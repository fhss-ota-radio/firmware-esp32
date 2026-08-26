#include "ota_client_internal.h"

#include <inttypes.h>

#include "esp_log.h"
#include "fhss_config_store.h"
#include "ota_protocol.h"

#define OTA_CONSUMER_TASK_STACK_SIZE 4096U
#define OTA_CONSUMER_TASK_PRIORITY   (tskIDLE_PRIORITY + 3U)

static const char *TAG = "ota_consumer";

static void ota_consumer_emit_event(
    ota_client_context_t *context,
    ota_client_event_t event,
    esp_err_t error)
{
    if (context->config.event_callback != NULL) {
        context->config.event_callback(
            event, 0U, error, context->config.callback_context);
    }
}

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

static esp_err_t ota_consumer_send_batch_ack(
    ota_client_context_t *context,
    uint32_t session_id,
    uint32_t base_sequence,
    uint8_t chunk_count,
    uint8_t received_mask,
    ota_result_t result
)
{
    const ota_batch_ack_fields_t fields = {
        .session_id = session_id,
        .base_sequence = base_sequence,
        .chunk_count = chunk_count,
        .received_mask = received_mask,
        .result_code = (uint8_t)result,
    };
    uint8_t packet[OTA_BATCH_ACK_PACKET_SIZE];
    const size_t packet_length = ota_protocol_encode_batch_ack(
        packet, sizeof(packet), &fields);
    if (packet_length == 0U) {
        return ESP_FAIL;
    }
    ESP_LOGI(
        TAG,
        "TX BATCH_ACK: session=%" PRIu32 ", base=%" PRIu32
        ", count=%u, mask=0x%02X, result=%u",
        session_id, base_sequence, (unsigned)chunk_count,
        received_mask, (unsigned)result
    );
    return context->config.send_callback(
        packet, packet_length, context->config.callback_context);
}

static void ota_consumer_handle_discover(ota_client_context_t *context)
{
    if (!ota_consumer_is_ota_mode(context)) {
        ESP_LOGD(TAG, "RX DISCOVER ignored: product is not in OTA menu");
        return;
    }

    const uint32_t backoff_max_ms =
        context->config.discover_backoff_max_ms;
    if (backoff_max_ms > 0U) {
        const uint32_t delay_ms = context->config.random_callback(
            context->config.callback_context
        ) % (backoff_max_ms + 1U);
        if (delay_ms > 0U) {
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
        }
        /* 메뉴를 떠난 동안 예약됐던 응답은 보내지 않는다. DISCOVER 자체는
         * session이나 제품 FSM을 변경하지 않는다. */
        if (!ota_consumer_is_ota_mode(context)) {
            ESP_LOGD(TAG, "DISCOVER_ACK cancelled: product left OTA menu");
            return;
        }
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

static bool ota_consumer_target_matches(
    const ota_client_context_t *context,
    uint32_t target_device_id)
{
    return target_device_id == context->config.device_id ||
           target_device_id == OTA_BROADCAST_DEVICE_ID;
}

static void ota_consumer_handle_fhss_config(
    ota_client_context_t *context,
    const uint8_t *packet,
    size_t packet_length)
{
    ota_fhss_config_fields_t fields = {0};
    if (!ota_protocol_decode_fhss_config(packet, packet_length, &fields)) {
        ESP_LOGW(TAG, "FHSS_CONFIG rejected: wire decode failed, bytes=%u",
                 (unsigned)packet_length);
        return;
    }
    if (!ota_consumer_target_matches(context, fields.target_device_id)) {
        ESP_LOGW(TAG,
                 "FHSS_CONFIG ignored: target=%08" PRIX32
                 " local=%08" PRIX32,
                 fields.target_device_id, context->config.device_id);
        return;
    }
    if (!ota_consumer_is_ota_mode(context)) {
        ESP_LOGW(TAG, "FHSS_CONFIG rejected: device is not in OTA FSM mode");
        (void)ota_consumer_send_nack(
            context, fields.session_id, OTA_PKT_FHSS_CONFIG,
            OTA_CONTROL_SEQUENCE, OTA_RESULT_BUSY);
        return;
    }
    ESP_LOGI(TAG,
             "FHSS_CONFIG pending verified: session=%" PRIu32
             " generation=%" PRIu32 " seed=%08" PRIX32,
             fields.session_id, fields.generation, fields.seed);
    const esp_err_t err = fhss_config_store_save_pending(&fields);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "FHSS_CONFIG pending save failed: %s",
                 esp_err_to_name(err));
        (void)ota_consumer_send_nack(
            context, fields.session_id, OTA_PKT_FHSS_CONFIG,
            OTA_CONTROL_SEQUENCE, OTA_RESULT_VERIFY_FAILED);
        return;
    }
    if (ota_consumer_send_ack(
            context, fields.session_id, OTA_PKT_FHSS_CONFIG,
            OTA_CONTROL_SEQUENCE) == ESP_OK) {
        ota_consumer_emit_event(
            context, OTA_CLIENT_EVENT_FHSS_CONFIG_READY, ESP_OK);
    }
}

static void ota_consumer_handle_fhss_activate(
    ota_client_context_t *context,
    const uint8_t *packet,
    size_t packet_length)
{
    ota_fhss_activate_fields_t activate = {0};
    if (!ota_protocol_decode_fhss_activate(
            packet, packet_length, &activate)) {
        ESP_LOGW(TAG, "FHSS_ACTIVATE rejected: wire decode failed, bytes=%u",
                 (unsigned)packet_length);
        return;
    }
    if (!ota_consumer_target_matches(context, activate.target_device_id)) {
        ESP_LOGW(TAG,
                 "FHSS_ACTIVATE ignored: target=%08" PRIX32
                 " local=%08" PRIX32,
                 activate.target_device_id, context->config.device_id);
        return;
    }
    ota_fhss_config_fields_t pending = {0};
    if (fhss_config_store_load_pending(&pending) != ESP_OK ||
        pending.generation != activate.generation ||
        pending.session_id != activate.session_id) {
        ESP_LOGW(TAG,
                 "FHSS_ACTIVATE rejected: pending(session=%" PRIu32
                 ",gen=%" PRIu32 ") rx(session=%" PRIu32
                 ",gen=%" PRIu32 ")",
                 pending.session_id, pending.generation,
                 activate.session_id, activate.generation);
        (void)ota_consumer_send_nack(
            context, activate.session_id, OTA_PKT_FHSS_ACTIVATE,
            OTA_CONTROL_SEQUENCE, OTA_RESULT_INVALID_SEQUENCE);
        return;
    }

    /* The ACK must leave on bootstrap channel 0 before the callback switches
     * the shared CC1101 to rendezvous channel 1. */
    if (ota_consumer_send_ack(
            context, activate.session_id, OTA_PKT_FHSS_ACTIVATE,
            OTA_CONTROL_SEQUENCE) != ESP_OK) {
        return;
    }
    ota_consumer_emit_event(
        context, OTA_CLIENT_EVENT_FHSS_ACTIVATING, ESP_OK);
    ESP_LOGI(TAG,
             "FHSS_ACTIVATE ACK sent; switching to rendezvous=%u generation=%" PRIu32,
             pending.rendezvous_channel, pending.generation);
    if (context->config.fhss_activate_callback == NULL ||
        context->config.fhss_activate_callback(
            &pending, context->config.callback_context) != ESP_OK) {
        ESP_LOGE(TAG, "FHSS activation failed after ACK; bootstrap recovery required");
        ota_consumer_emit_event(
            context, OTA_CLIENT_EVENT_FHSS_ACTIVATE_FAILED, ESP_FAIL);
        return;
    }
    /* Keep it pending until the radio service proves this generation by
     * acquiring valid SYNC packets. */
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
        ESP_LOGW(TAG, "DATA dropped before BATCH_END: decode result=%u",
                 (unsigned)ota_consumer_data_decode_error(packet, packet_length));
        return;
    }
    if (payload_length == 0U) {
        ESP_LOGW(TAG, "DATA dropped before BATCH_END: zero payload seq=%" PRIu32,
                 header.sequence);
        return;
    }

    if (context->state != OTA_CLIENT_STATE_RECEIVING) {
        ESP_LOGW(TAG, "DATA dropped: receiver busy seq=%" PRIu32,
                 header.sequence);
        return;
    }
    if (header.session_id != context->session_id) {
        ESP_LOGW(TAG, "DATA dropped: invalid session seq=%" PRIu32,
                 header.sequence);
        return;
    }

    const esp_err_t err = ota_client_write_chunk(
        header.session_id,
        header.sequence,
        payload,
        payload_length
    );
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "DATA store failed before BATCH_END: seq=%" PRIu32
                 ", error=%s", header.sequence, esp_err_to_name(err));
        return;
    }

    ESP_LOGI(
        TAG,
        "DATA accepted: session=%" PRIu32 ", seq=%" PRIu32 ", bytes=%u",
        header.session_id,
        header.sequence,
        (unsigned)payload_length
    );

    /* DATA에는 즉시 응답하지 않는다. 명시적 BATCH_END를 받은 뒤 현재
     * received_mask 하나로 답해야 반이중 ACK TX가 다음 DATA RX와 겹치지
     * 않고, 마지막 DATA 유실도 배치 경계와 분리해 판단할 수 있다. */
    ESP_LOGI(TAG, "DATA response deferred until BATCH_END: seq=%" PRIu32,
             header.sequence);
}

static void ota_consumer_handle_batch_end(
    ota_client_context_t *context,
    const uint8_t *packet,
    size_t packet_length)
{
    ota_batch_end_fields_t fields;
    if (!ota_protocol_decode_batch_end(packet, packet_length, &fields)) {
        return;
    }

    uint8_t received_mask = 0U;
    ota_result_t result = OTA_RESULT_OK;
    const uint8_t required_mask =
        ota_protocol_batch_required_mask(fields.chunk_count);

    if (context->state != OTA_CLIENT_STATE_RECEIVING) {
        result = OTA_RESULT_BUSY;
    } else if (fields.session_id != context->session_id) {
        result = OTA_RESULT_INVALID_SESSION;
    } else if (fields.base_sequence > context->total_chunks ||
               fields.chunk_count > context->total_chunks - fields.base_sequence) {
        result = OTA_RESULT_INVALID_SEQUENCE;
    } else if (fields.base_sequence < context->expected_sequence &&
               fields.base_sequence + fields.chunk_count <=
                   context->expected_sequence) {
        /* Flash commit 완료 뒤 BATCH_ACK만 유실된 재요청. 같은 full mask를
         * 재응답하고 Flash에는 절대 다시 쓰지 않는다. */
        received_mask = required_mask;
    } else if (fields.base_sequence == context->batch_cache.base_sequence &&
               fields.chunk_count == context->batch_cache.chunk_count) {
        received_mask = (uint8_t)(
            context->batch_cache.received_mask & required_mask);
    } else {
        result = OTA_RESULT_INVALID_SEQUENCE;
    }

    (void)ota_consumer_send_batch_ack(
        context, fields.session_id, fields.base_sequence,
        fields.chunk_count, received_mask, result);
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
        case OTA_PKT_BATCH_END:
            ota_consumer_handle_batch_end(
                context, queued_packet->data, queued_packet->length
            );
            break;
        case OTA_PKT_END:
            ota_consumer_handle_end(
                context, queued_packet->data, queued_packet->length
            );
            break;
        case OTA_PKT_FHSS_CONFIG:
            ota_consumer_handle_fhss_config(
                context, queued_packet->data, queued_packet->length);
            break;
        case OTA_PKT_FHSS_ACTIVATE:
            ota_consumer_handle_fhss_activate(
                context, queued_packet->data, queued_packet->length);
            break;
        case OTA_PKT_FHSS_SYNC:
            /* Runtime SYNC belongs to fhss_service, not the OTA consumer. */
            break;
        case OTA_PKT_ACK:
        case OTA_PKT_NACK:
        case OTA_PKT_DISCOVER_ACK:
        case OTA_PKT_BATCH_ACK:
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
    const TickType_t timeout_ticks = pdMS_TO_TICKS(
        context->config.receive_timeout_ms
    );
    for (;;) {
        TickType_t receive_wait = timeout_ticks;
        if (context->state == OTA_CLIENT_STATE_RECEIVING) {
            const TickType_t elapsed =
                xTaskGetTickCount() - context->last_packet_tick;
            receive_wait = elapsed >= timeout_ticks
                ? 0U
                : timeout_ticks - elapsed;
        }
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
