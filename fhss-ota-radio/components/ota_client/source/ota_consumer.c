#include "ota_client_internal.h"

#include <inttypes.h>

#include "esp_log.h"
#include "ota_protocol.h"

#define OTA_CONSUMER_TASK_STACK_SIZE 4096U
#define OTA_CONSUMER_TASK_PRIORITY   (tskIDLE_PRIORITY + 3U)

static const char *TAG = "ota_consumer";

static bool ota_consumer_is_ota_mode(const ota_client_context_t *context)
{
    return context->config.ota_mode_callback != NULL &&
           context->config.ota_mode_callback(
               context->config.callback_context
           );
}

static esp_err_t ota_consumer_send_response(
    ota_client_context_t *context,
    ota_packet_type_t response_type,
    uint32_t session_id,
    ota_packet_type_t acknowledged_type,
    uint32_t sequence,
    ota_result_t result
)
{
    const ota_ack_fields_t fields = {
        .session_id = session_id,
        .acknowledged_type = (uint8_t)acknowledged_type,
        .sequence = sequence,
        .result_code = (uint8_t)result,
    };
    uint8_t packet[OTA_ACK_PACKET_SIZE];
    const size_t packet_length = ota_protocol_encode_ack(
        packet,
        sizeof(packet),
        response_type,
        &fields
    );
    if (packet_length == 0U) {
        return ESP_FAIL;
    }

    ESP_LOGI(
        TAG,
        "TX %s: session=%" PRIu32 ", ack_type=%u, seq=%" PRIu32
        ", result=%u",
        response_type == OTA_PKT_ACK ? "ACK" : "NACK",
        session_id,
        (unsigned)acknowledged_type,
        sequence,
        (unsigned)result
    );

    const esp_err_t err = context->config.send_callback(
        packet,
        packet_length,
        context->config.callback_context
    );
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "response send failed: %s", esp_err_to_name(err));
    }
    return err;
}

static esp_err_t ota_consumer_send_ack(
    ota_client_context_t *context,
    uint32_t session_id,
    ota_packet_type_t acknowledged_type,
    uint32_t sequence
)
{
    return ota_consumer_send_response(
        context,
        OTA_PKT_ACK,
        session_id,
        acknowledged_type,
        sequence,
        OTA_RESULT_OK
    );
}

static esp_err_t ota_consumer_send_nack(
    ota_client_context_t *context,
    uint32_t session_id,
    ota_packet_type_t acknowledged_type,
    uint32_t sequence,
    ota_result_t result
)
{
    return ota_consumer_send_response(
        context,
        OTA_PKT_NACK,
        session_id,
        acknowledged_type,
        sequence,
        result
    );
}

static void ota_consumer_handle_discover(ota_client_context_t *context)
{
    if (!ota_consumer_is_ota_mode(context)) {
        ESP_LOGD(TAG, "RX DISCOVER ignored: product is not in OTA menu");
        return;
    }

    const ota_discover_ack_fields_t fields = {
        .device_id = context->config.device_id,
        .fw_major = context->config.firmware_version[0],
        .fw_minor = context->config.firmware_version[1],
        .fw_patch = context->config.firmware_version[2],
    };
    uint8_t response[OTA_DISCOVER_ACK_PACKET_SIZE];
    const size_t response_length = ota_protocol_encode_discover_ack(
        response,
        sizeof(response),
        &fields
    );
    if (response_length == 0U) {
        ESP_LOGW(TAG, "DISCOVER_ACK encode failed");
        return;
    }

    const esp_err_t err = context->config.send_callback(
        response,
        response_length,
        context->config.callback_context
    );
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "DISCOVER_ACK send failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(
            TAG,
            "TX DISCOVER_ACK: device=%06" PRIX32 ", fw=%u.%u.%u",
            fields.device_id,
            fields.fw_major,
            fields.fw_minor,
            fields.fw_patch
        );
    }
}

static void ota_consumer_handle_start(
    ota_client_context_t *context,
    const uint8_t *packet,
    size_t packet_length
)
{
    ota_start_fields_t fields;
    if (!ota_protocol_decode_start(packet, packet_length, &fields)) {
        if (packet_length >= 5U) {
            ota_consumer_send_nack(
                context,
                ota_read_u32_le(&packet[1]),
                OTA_PKT_START,
                OTA_CONTROL_SEQUENCE,
                OTA_RESULT_INVALID_SIZE
            );
        }
        return;
    }

    if (fields.target_device_id != context->config.device_id &&
        fields.target_device_id != OTA_BROADCAST_DEVICE_ID) {
        /* 다른 단말 대상 START에는 여러 기기가 동시에 NACK하지 않는다. */
        ESP_LOGD(
            TAG,
            "RX START ignored: target=%08" PRIX32,
            fields.target_device_id
        );
        return;
    }
    /* START ACK 유실로 같은 START가 재전송된 경우 새 writer를 열지 않는다. */
    if (context->state == OTA_CLIENT_STATE_RECEIVING &&
        fields.session_id == context->session_id &&
        fields.image_size == context->image_size &&
        fields.total_chunks == context->total_chunks) {
        ota_consumer_send_ack(
            context,
            fields.session_id,
            OTA_PKT_START,
            OTA_CONTROL_SEQUENCE
        );
        return;
    }
    if (!ota_consumer_is_ota_mode(context)) {
        ota_consumer_send_nack(
            context,
            fields.session_id,
            OTA_PKT_START,
            OTA_CONTROL_SEQUENCE,
            OTA_RESULT_BUSY
        );
        return;
    }

    const esp_err_t err = ota_client_start_session(
        fields.session_id,
        fields.image_size,
        fields.total_chunks,
        fields.image_sha256
    );
    if (err != ESP_OK) {
        ota_consumer_send_nack(
            context,
            fields.session_id,
            OTA_PKT_START,
            OTA_CONTROL_SEQUENCE,
            err == ESP_ERR_INVALID_SIZE
                ? OTA_RESULT_INVALID_SIZE
                : (err == ESP_ERR_INVALID_STATE
                    ? OTA_RESULT_BUSY
                    : OTA_RESULT_WRITE_FAILED)
        );
        return;
    }

    ESP_LOGI(
        TAG,
        "START accepted: session=%" PRIu32 ", image=%" PRIu32
        ", chunks=%" PRIu32,
        fields.session_id,
        fields.image_size,
        fields.total_chunks
    );

    ota_consumer_send_ack(
        context,
        fields.session_id,
        OTA_PKT_START,
        OTA_CONTROL_SEQUENCE
    );
}

static ota_result_t ota_consumer_data_decode_error(
    const uint8_t *packet,
    size_t packet_length
)
{
    if (packet_length < OTA_DATA_HEADER_SIZE) {
        return OTA_RESULT_INVALID_SIZE;
    }

    const size_t payload_length = packet[9];
    if (payload_length > OTA_MAX_PAYLOAD_SIZE ||
        packet_length != OTA_DATA_HEADER_SIZE + payload_length) {
        return OTA_RESULT_INVALID_SIZE;
    }

    return OTA_RESULT_INVALID_CRC;
}

static void ota_consumer_handle_data(
    ota_client_context_t *context,
    const uint8_t *packet,
    size_t packet_length
)
{
    ota_data_header_fields_t header;
    const uint8_t *payload = NULL;
    size_t payload_length = 0U;
    const bool decoded = ota_protocol_decode_data(
        packet,
        packet_length,
        &header,
        &payload,
        &payload_length
    );

    /* 공용 decoder는 최소 길이를 허용하므로 wire packet은 정확한 길이인지도
     * 세션 계층에서 확인한다. */
    const bool exact_length = decoded &&
        packet_length == OTA_DATA_HEADER_SIZE + payload_length;
    if (!exact_length) {
        if (packet_length >= 9U) {
            ota_consumer_send_nack(
                context,
                ota_read_u32_le(&packet[1]),
                OTA_PKT_DATA,
                ota_read_u32_le(&packet[5]),
                ota_consumer_data_decode_error(packet, packet_length)
            );
        }
        return;
    }
    if (payload_length == 0U) {
        ota_consumer_send_nack(
            context,
            header.session_id,
            OTA_PKT_DATA,
            header.sequence,
            OTA_RESULT_INVALID_SIZE
        );
        return;
    }

    if (context->state != OTA_CLIENT_STATE_RECEIVING) {
        ota_consumer_send_nack(
            context,
            header.session_id,
            OTA_PKT_DATA,
            header.sequence,
            OTA_RESULT_BUSY
        );
        return;
    }
    if (header.session_id != context->session_id) {
        ota_consumer_send_nack(
            context,
            header.session_id,
            OTA_PKT_DATA,
            header.sequence,
            OTA_RESULT_INVALID_SESSION
        );
        return;
    }

    const esp_err_t err = ota_client_write_chunk(
        header.session_id,
        header.sequence,
        payload,
        payload_length
    );
    if (err != ESP_OK) {
        ota_consumer_send_nack(
            context,
            header.session_id,
            OTA_PKT_DATA,
            header.sequence,
            err == ESP_ERR_INVALID_STATE
                ? OTA_RESULT_BUSY
                : (err == ESP_ERR_INVALID_ARG
                    ? OTA_RESULT_INVALID_SEQUENCE
                    : OTA_RESULT_WRITE_FAILED)
        );
        return;
    }

    ESP_LOGI(
        TAG,
        "DATA accepted: session=%" PRIu32 ", seq=%" PRIu32 ", bytes=%u",
        header.session_id,
        header.sequence,
        (unsigned)payload_length
    );

    ota_consumer_send_ack(
        context,
        header.session_id,
        OTA_PKT_DATA,
        header.sequence
    );
}

static void ota_consumer_handle_end(
    ota_client_context_t *context,
    const uint8_t *packet,
    size_t packet_length
)
{
    ota_end_fields_t fields;
    if (!ota_protocol_decode_end(packet, packet_length, &fields)) {
        if (packet_length >= 5U) {
            ota_consumer_send_nack(
                context,
                ota_read_u32_le(&packet[1]),
                OTA_PKT_END,
                OTA_CONTROL_SEQUENCE,
                OTA_RESULT_INVALID_SIZE
            );
        }
        return;
    }

    /* END ACK 유실 뒤 동일 END가 다시 들어오면 검증/파티션 설정을 반복하지 않는다. */
    if (context->state == OTA_CLIENT_STATE_READY_TO_REBOOT &&
        fields.session_id == context->session_id &&
        fields.image_size == context->image_size &&
        fields.total_chunks == context->total_chunks) {
        ota_consumer_send_ack(
            context,
            fields.session_id,
            OTA_PKT_END,
            OTA_CONTROL_SEQUENCE
        );
        return;
    }
    if (context->state != OTA_CLIENT_STATE_RECEIVING) {
        ota_consumer_send_nack(
            context,
            fields.session_id,
            OTA_PKT_END,
            OTA_CONTROL_SEQUENCE,
            OTA_RESULT_BUSY
        );
        return;
    }
    if (fields.session_id != context->session_id) {
        ota_consumer_send_nack(
            context,
            fields.session_id,
            OTA_PKT_END,
            OTA_CONTROL_SEQUENCE,
            OTA_RESULT_INVALID_SESSION
        );
        return;
    }

    const esp_err_t err = ota_client_finish_session(
        fields.session_id,
        fields.image_size,
        fields.total_chunks
    );
    if (err != ESP_OK) {
        ota_consumer_send_nack(
            context,
            fields.session_id,
            OTA_PKT_END,
            OTA_CONTROL_SEQUENCE,
            err == ESP_ERR_INVALID_SIZE
                ? OTA_RESULT_INVALID_SIZE
                : OTA_RESULT_VERIFY_FAILED
        );
        return;
    }

    ESP_LOGI(TAG, "END verified: session=%" PRIu32, fields.session_id);

    ota_consumer_send_ack(
        context,
        fields.session_id,
        OTA_PKT_END,
        OTA_CONTROL_SEQUENCE
    );
}

static void ota_consumer_process_packet(
    ota_client_context_t *context,
    const ota_client_rx_packet_t *queued_packet
)
{
    ota_packet_type_t type;
    if (!ota_protocol_peek_type(
            queued_packet->data,
            queued_packet->length,
            &type
        )) {
        return;
    }

    ESP_LOGI(
        TAG,
        "RX packet: type=%u, bytes=%u",
        (unsigned)type,
        (unsigned)queued_packet->length
    );

    switch (type) {
        case OTA_PKT_DISCOVER:
            if (ota_protocol_decode_discover(
                    queued_packet->data,
                    queued_packet->length
                )) {
                ota_consumer_handle_discover(context);
            }
            break;
        case OTA_PKT_START:
            ota_consumer_handle_start(
                context, queued_packet->data, queued_packet->length
            );
            break;
        case OTA_PKT_DATA:
            ota_consumer_handle_data(
                context, queued_packet->data, queued_packet->length
            );
            break;
        case OTA_PKT_END:
            ota_consumer_handle_end(
                context, queued_packet->data, queued_packet->length
            );
            break;
        case OTA_PKT_ACK:
        case OTA_PKT_NACK:
        case OTA_PKT_DISCOVER_ACK:
        default:
            /* 이 방향의 패킷은 Gateway가 소비한다. */
            break;
    }
}

static void ota_consumer_handle_receive_timeout(
    ota_client_context_t *context
)
{
    if (context->state != OTA_CLIENT_STATE_RECEIVING) {
        return;
    }

    const TickType_t now = xTaskGetTickCount();
    const TickType_t timeout_ticks = pdMS_TO_TICKS(
        context->config.receive_timeout_ms
    );
    if ((now - context->last_packet_tick) < timeout_ticks) {
        return;
    }

    const uint8_t missing_mask = ota_batch_cache_missing_mask(
        &context->batch_cache
    );
    ESP_LOGW(
        TAG,
        "RX timeout: session=%" PRIu32 ", batch_base=%" PRIu32
        ", missing_mask=0x%02X",
        context->session_id,
        context->batch_cache.base_sequence,
        missing_mask
    );
    for (uint8_t offset = 0U;
         offset < context->batch_cache.chunk_count;
         ++offset) {
        if ((missing_mask & (uint8_t)(1U << offset)) != 0U) {
            ota_consumer_send_nack(
                context,
                context->session_id,
                OTA_PKT_DATA,
                context->batch_cache.base_sequence + offset,
                OTA_RESULT_TIMEOUT
            );
        }
    }
    context->last_packet_tick = now;
}

static void ota_consumer_task(void *arg)
{
    ota_client_context_t *context = (ota_client_context_t *)arg;
    ota_client_rx_packet_t packet;

    ESP_LOGI(TAG, "consumer task started");
    const TickType_t receive_wait = pdMS_TO_TICKS(
        context->config.receive_timeout_ms
    );
    for (;;) {
        if (ota_client_receive_packet(&packet, receive_wait) == ESP_OK) {
            ota_consumer_process_packet(context, &packet);
        } else {
            ota_consumer_handle_receive_timeout(context);
        }
    }
}

esp_err_t ota_consumer_start(ota_client_context_t *context)
{
    if (context == NULL || context->consumer_task != NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const BaseType_t created = xTaskCreate(
        ota_consumer_task,
        "ota_consumer",
        OTA_CONSUMER_TASK_STACK_SIZE,
        context,
        OTA_CONSUMER_TASK_PRIORITY,
        &context->consumer_task
    );
    return created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
