#include <inttypes.h>
#include <string.h>

#include "esp_log.h"
#include "ota_client.h"
#include "ota_client_internal.h"
#include "freertos/queue.h"
#include "fhss_config_store.h"
#include "ota_protocol.h"

static ota_client_context_t s_ota_client;
static const char *TAG = "ota_client";

static uint32_t ota_client_calculate_total_chunks(uint32_t image_size)
{
    return (image_size / OTA_CLIENT_DATA_MAX_PAYLOAD_SIZE) +
           ((image_size % OTA_CLIENT_DATA_MAX_PAYLOAD_SIZE) != 0U ? 1U : 0U);
}

esp_err_t ota_client_init(const ota_client_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (config->send_callback == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (config->device_id > OTA_DEVICE_ID_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (config->receive_timeout_ms == 0 ||
        config->discover_backoff_max_ms == UINT32_MAX ||
        (config->discover_backoff_max_ms > 0U &&
         config->random_callback == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_ota_client.state != OTA_CLIENT_STATE_UNINITIALIZED) {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t store_err = fhss_config_store_init();
    if (store_err != ESP_OK) {
        return store_err;
    }

    memset(&s_ota_client, 0, sizeof(s_ota_client));

    s_ota_client.rx_queue = xQueueCreate(
        OTA_CLIENT_RX_QUEUE_DEPTH,
        sizeof(ota_client_rx_packet_t)
    );
    if (s_ota_client.rx_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_ota_client.config = *config;
    s_ota_client.state = OTA_CLIENT_STATE_IDLE;

    return ESP_OK;
}

ota_client_state_t ota_client_get_state(void)
{
    return s_ota_client.state;
}


// packet type : OTA_PACKET_START
esp_err_t ota_client_start_session(
    uint32_t session_id,
    uint32_t image_size,
    uint32_t total_chunks,
    const uint8_t expected_sha256[32]
) {
    if (image_size == 0U || total_chunks == 0U || expected_sha256 == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (total_chunks != ota_client_calculate_total_chunks(image_size)) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (s_ota_client.state != OTA_CLIENT_STATE_IDLE) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = ota_writer_begin(&s_ota_client.writer, image_size);

    if (err != ESP_OK) {
        s_ota_client.state = OTA_CLIENT_STATE_ERROR;

        if (s_ota_client.config.event_callback != NULL) {
            s_ota_client.config.event_callback(
                OTA_CLIENT_EVENT_FAILED,
                0,
                err,
                s_ota_client.config.callback_context
            );
        }

        return err;
    }     

    s_ota_client.session_id = session_id;
    s_ota_client.image_size = image_size;
    s_ota_client.received_bytes = 0;
    s_ota_client.total_chunks = total_chunks;
    s_ota_client.expected_sequence = 0;
    memcpy(
        s_ota_client.expected_sha256,
        expected_sha256,
        sizeof(s_ota_client.expected_sha256)
    );
    ota_batch_cache_prepare(
        &s_ota_client.batch_cache,
        s_ota_client.expected_sequence,
        s_ota_client.total_chunks
    );
    s_ota_client.last_packet_tick = xTaskGetTickCount();
    s_ota_client.state = OTA_CLIENT_STATE_RECEIVING;

    if (s_ota_client.config.event_callback != NULL) {
        s_ota_client.config.event_callback(
            OTA_CLIENT_EVENT_STARTED,
            0,
            ESP_OK,
            s_ota_client.config.callback_context
        );
    }

    return ESP_OK;
}

static esp_err_t ota_client_batch_write(
    const uint8_t *data,
    size_t data_size,
    void *context
)
{
    return ota_writer_write((ota_writer_t *)context, data, data_size);
}

// 수신한 packet type : OTA_PACKET_DATA 일 때
esp_err_t ota_client_write_chunk(
    uint32_t session_id,
    uint32_t sequence,
    const uint8_t *data,
    size_t data_size
)
{
    if (data == NULL || data_size == 0U ||
        data_size > OTA_CLIENT_DATA_MAX_PAYLOAD_SIZE) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_ota_client.state != OTA_CLIENT_STATE_RECEIVING) {
        return ESP_ERR_INVALID_STATE;
    }

    if (session_id != s_ota_client.session_id) {
        return ESP_ERR_INVALID_ARG;
    }

    if (sequence >= s_ota_client.total_chunks) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint64_t chunk_offset =
        (uint64_t)sequence * OTA_CLIENT_DATA_MAX_PAYLOAD_SIZE;
    const size_t expected_data_size =
        sequence + 1U == s_ota_client.total_chunks
            ? (size_t)((uint64_t)s_ota_client.image_size - chunk_offset)
            : OTA_CLIENT_DATA_MAX_PAYLOAD_SIZE;
    if (data_size != expected_data_size) {
        /* 중간 청크는 항상 48B, 이미지의 마지막 청크만 잔여 길이를 허용한다.
         * 그래야 batch cache를 연속 buffer로 한 번에 Flash에 기록할 수 있다. */
        return ESP_ERR_INVALID_SIZE;
    }
    if (sequence < s_ota_client.expected_sequence) {
        /* ACK 유실로 이전 배치 DATA가 재전송된 경우 안전하게 무시한다. */
        return ESP_OK;
    }

    bool duplicate = false;
    const esp_err_t store_err = ota_batch_cache_store(
        &s_ota_client.batch_cache,
        sequence,
        data,
        data_size,
        &duplicate
    );
    if (store_err != ESP_OK) {
        return store_err;
    }

    if (duplicate) {
        /* 이미 받은 slot의 반복 패킷으로 다른 missing slot의 timeout/NACK를
         * 무기한 미루지 않는다. ACK 유실 복구를 위해 호출 자체는 성공이다. */
        ESP_LOGI(
            TAG,
            "batch duplicate: base=%" PRIu32 ", seq=%" PRIu32
            ", received=0x%02X, missing=0x%02X",
            s_ota_client.batch_cache.base_sequence,
            sequence,
            s_ota_client.batch_cache.received_mask,
            ota_batch_cache_missing_mask(&s_ota_client.batch_cache)
        );
        return ESP_OK;
    }

    s_ota_client.last_packet_tick = xTaskGetTickCount();

    ESP_LOGI(
        TAG,
        "batch store: base=%" PRIu32 ", seq=%" PRIu32
        ", received=0x%02X, missing=0x%02X",
        s_ota_client.batch_cache.base_sequence,
        sequence,
        s_ota_client.batch_cache.received_mask,
        ota_batch_cache_missing_mask(&s_ota_client.batch_cache)
    );

    /* 현재 고정 배치가 완성되면 연속된 최대 240B를 Flash에 한 번 기록한다.
     * 응답은 consumer가 뒤이어 수신한 BATCH_END에 대해 BATCH_ACK bitmap
     * 하나로 보낸다. */
    if (!ota_batch_cache_is_complete(&s_ota_client.batch_cache)) {
        return ESP_OK;
    }

    ESP_LOGI(
        TAG,
        "batch complete: base=%" PRIu32 ", chunks=%u; committing to Flash",
        s_ota_client.batch_cache.base_sequence,
        (unsigned)s_ota_client.batch_cache.chunk_count
    );

    size_t batch_bytes = 0U;
    const esp_err_t write_err = ota_batch_cache_commit(
        &s_ota_client.batch_cache,
        ota_client_batch_write,
        &s_ota_client.writer,
        &batch_bytes
    );
    if (write_err != ESP_OK) {
        ota_writer_abort(&s_ota_client.writer);
        s_ota_client.state = OTA_CLIENT_STATE_ERROR;

        if (s_ota_client.config.event_callback != NULL) {
            s_ota_client.config.event_callback(
                OTA_CLIENT_EVENT_FAILED,
                0,
                write_err,
                s_ota_client.config.callback_context
            );
        }
        return write_err;
    }

    s_ota_client.received_bytes += batch_bytes;
    s_ota_client.expected_sequence += s_ota_client.batch_cache.chunk_count;

    ESP_LOGI(
        TAG,
        "batch committed: bytes=%u, received_bytes=%" PRIu32
        ", next_sequence=%" PRIu32,
        (unsigned)batch_bytes,
        s_ota_client.received_bytes,
        s_ota_client.expected_sequence
    );

    ota_batch_cache_prepare(
        &s_ota_client.batch_cache,
        s_ota_client.expected_sequence,
        s_ota_client.total_chunks
    );

    uint32_t progress = (uint32_t)(
        ((uint64_t)s_ota_client.received_bytes * 100U) /
        s_ota_client.image_size
    );

    if (s_ota_client.config.event_callback != NULL) {
        s_ota_client.config.event_callback(
            OTA_CLIENT_EVENT_PROGRESS,
            progress,
            ESP_OK,
            s_ota_client.config.callback_context
        );
    }

    return ESP_OK;
}

esp_err_t ota_client_finish_session(
    uint32_t session_id,
    uint32_t image_size,
    uint32_t total_chunks
)
{
    if (s_ota_client.state != OTA_CLIENT_STATE_RECEIVING) {
        return ESP_ERR_INVALID_STATE;
    }

    if (session_id != s_ota_client.session_id) {
        return ESP_ERR_INVALID_ARG;
    }
    if (image_size != s_ota_client.image_size ||
        total_chunks != s_ota_client.total_chunks) {
        return ESP_ERR_INVALID_SIZE;
    }

    /*
     * END가 너무 일찍 도착했다면 세션을 종료하지 않는다.
     * 나중에 누락된 sequence를 NACK할 수 있도록 RECEIVING을 유지한다.
     */
    if (s_ota_client.received_bytes != s_ota_client.image_size ||
        s_ota_client.expected_sequence != s_ota_client.total_chunks) {
        return ESP_ERR_INVALID_SIZE;
    }

    s_ota_client.state = OTA_CLIENT_STATE_VERIFYING;

    if (s_ota_client.config.event_callback != NULL) {
        s_ota_client.config.event_callback(
            OTA_CLIENT_EVENT_APPLYING,
            100,
            ESP_OK,
            s_ota_client.config.callback_context
        );
    }

    esp_err_t err = ota_writer_finish(
        &s_ota_client.writer,
        s_ota_client.expected_sha256
    );

    if (err != ESP_OK) {
        s_ota_client.state = OTA_CLIENT_STATE_ERROR;

        if (s_ota_client.config.event_callback != NULL) {
            s_ota_client.config.event_callback(
                OTA_CLIENT_EVENT_FAILED,
                100,
                err,
                s_ota_client.config.callback_context
            );
        }

        return err;
    }

    s_ota_client.state = OTA_CLIENT_STATE_READY_TO_REBOOT;

    if (s_ota_client.config.event_callback != NULL) {
        s_ota_client.config.event_callback(
            OTA_CLIENT_EVENT_COMPLETED,
            100,
            ESP_OK,
            s_ota_client.config.callback_context
        );
    }

    return ESP_OK;
}

static void ota_client_reset_session(void)
{
    memset(&s_ota_client.writer, 0, sizeof(s_ota_client.writer));

    s_ota_client.session_id = 0;
    s_ota_client.image_size = 0;
    s_ota_client.received_bytes = 0;
    s_ota_client.total_chunks = 0;
    s_ota_client.expected_sequence = 0;
    memset(&s_ota_client.batch_cache, 0, sizeof(s_ota_client.batch_cache));
    memset(
        s_ota_client.expected_sha256,
        0,
        sizeof(s_ota_client.expected_sha256)
    );
    s_ota_client.last_packet_tick = 0;
}

esp_err_t ota_client_abort(void)
{
    if (s_ota_client.state != OTA_CLIENT_STATE_RECEIVING &&
        s_ota_client.state != OTA_CLIENT_STATE_ERROR) {
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t progress = 0;

    if (s_ota_client.image_size != 0) {
        progress = (uint32_t)(
            ((uint64_t)s_ota_client.received_bytes * 100U) /
            s_ota_client.image_size
        );
    }

    esp_err_t err = ESP_OK;

    if (s_ota_client.writer.active) {
        err = ota_writer_abort(&s_ota_client.writer);
    }

    if (s_ota_client.rx_queue != NULL) {
        xQueueReset(s_ota_client.rx_queue);
    }

    ota_client_reset_session();
    s_ota_client.state = OTA_CLIENT_STATE_IDLE;

    if (s_ota_client.config.event_callback != NULL) {
        s_ota_client.config.event_callback(
            OTA_CLIENT_EVENT_ABORTED,
            progress,
            err,
            s_ota_client.config.callback_context
        );
    }

    return err;
}

esp_err_t ota_client_submit_packet(
    const uint8_t *packet,
    size_t packet_length
)
{
    if (packet == NULL || packet_length == 0U ||
        packet_length > OTA_CLIENT_MAX_PACKET_LENGTH) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_ota_client.state == OTA_CLIENT_STATE_UNINITIALIZED ||
        s_ota_client.rx_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    ota_client_rx_packet_t queued_packet = {
        .length = (uint8_t)packet_length,
    };
    memcpy(queued_packet.data, packet, packet_length);

    if (xQueueSend(s_ota_client.rx_queue, &queued_packet, 0U) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}

esp_err_t ota_client_receive_packet(
    ota_client_rx_packet_t *out_packet,
    TickType_t wait_ticks
)
{
    if (out_packet == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_ota_client.state == OTA_CLIENT_STATE_UNINITIALIZED ||
        s_ota_client.rx_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    return xQueueReceive(s_ota_client.rx_queue, out_packet, wait_ticks) == pdTRUE
        ? ESP_OK
        : ESP_ERR_TIMEOUT;
}

esp_err_t ota_client_start_consumer(void)
{
    if (s_ota_client.state == OTA_CLIENT_STATE_UNINITIALIZED ||
        s_ota_client.rx_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_ota_client.consumer_task != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    return ota_consumer_start(&s_ota_client);
}
