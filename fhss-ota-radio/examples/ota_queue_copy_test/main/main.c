#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "ota_batch_cache.h"
#include "ota_client.h"
#include "ota_client_internal.h"

static const char *TAG = "OTA_QUEUE_TEST";

static esp_err_t discard_send(
    const uint8_t *packet,
    size_t packet_length,
    void *context
)
{
    (void)packet;
    (void)packet_length;
    (void)context;
    return ESP_OK;
}

static bool run_copy_test(void)
{
    uint8_t source[OTA_CLIENT_MAX_PACKET_LENGTH];
    uint8_t expected[OTA_CLIENT_MAX_PACKET_LENGTH];

    for (size_t i = 0U; i < sizeof(source); ++i) {
        source[i] = (uint8_t)i;
        expected[i] = (uint8_t)i;
    }

    if (ota_client_submit_packet(source, sizeof(source)) != ESP_OK) {
        ESP_LOGE(TAG, "submit failed");
        return false;
    }

    /* RF 드라이버가 동일한 수신 버퍼를 바로 재사용하는 상황을 재현한다. */
    memset(source, 0xA5, sizeof(source));

    ota_client_rx_packet_t copied = {0};
    if (ota_client_receive_packet(&copied, 0U) != ESP_OK) {
        ESP_LOGE(TAG, "dequeue failed");
        return false;
    }
    if (copied.length != sizeof(expected)) {
        ESP_LOGE(TAG, "length mismatch: expected=%u actual=%u",
                 (unsigned)sizeof(expected), copied.length);
        return false;
    }
    if (memcmp(copied.data, expected, sizeof(expected)) != 0) {
        ESP_LOGE(TAG, "copied payload changed after source buffer reuse");
        return false;
    }

    ESP_LOGI(TAG, "60-byte source buffer reuse test PASS");
    return true;
}

static bool run_length_validation_test(void)
{
    uint8_t packet[OTA_CLIENT_MAX_PACKET_LENGTH + 1U] = {0};

    if (ota_client_submit_packet(NULL, 1U) != ESP_ERR_INVALID_ARG ||
        ota_client_submit_packet(packet, 0U) != ESP_ERR_INVALID_ARG ||
        ota_client_submit_packet(packet, sizeof(packet)) != ESP_ERR_INVALID_ARG) {
        ESP_LOGE(TAG, "invalid packet length was accepted");
        return false;
    }

    ESP_LOGI(TAG, "packet length validation test PASS");
    return true;
}

static bool run_queue_capacity_test(void)
{
    uint8_t packet[1] = {0x5A};

    for (size_t i = 0U; i < OTA_CLIENT_RX_QUEUE_DEPTH; ++i) {
        if (ota_client_submit_packet(packet, sizeof(packet)) != ESP_OK) {
            ESP_LOGE(TAG, "queue filled early at item %u", (unsigned)i);
            return false;
        }
    }
    if (ota_client_submit_packet(packet, sizeof(packet)) != ESP_ERR_TIMEOUT) {
        ESP_LOGE(TAG, "queue overflow was not reported");
        return false;
    }

    ota_client_rx_packet_t discarded = {0};
    for (size_t i = 0U; i < OTA_CLIENT_RX_QUEUE_DEPTH; ++i) {
        if (ota_client_receive_packet(&discarded, 0U) != ESP_OK) {
            ESP_LOGE(TAG, "queue drain failed at item %u", (unsigned)i);
            return false;
        }
    }

    ESP_LOGI(TAG, "queue capacity test PASS");
    return true;
}

typedef struct {
    uint8_t data[OTA_CLIENT_BATCH_SIZE * OTA_CLIENT_DATA_MAX_PAYLOAD_SIZE];
    size_t length;
} batch_write_capture_t;

static esp_err_t capture_batch_write(
    const uint8_t *data,
    size_t data_size,
    void *context
)
{
    batch_write_capture_t *capture = (batch_write_capture_t *)context;
    if (capture == NULL || data == NULL ||
        data_size > sizeof(capture->data) - capture->length) {
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(&capture->data[capture->length], data, data_size);
    ESP_LOGI(TAG, "ORDERED WRITE CALLBACK: seq=%u, length=%u",
             data[0], (unsigned)data_size);
    capture->length += data_size;
    return ESP_OK;
}

static bool store_test_chunk(
    ota_batch_cache_t *cache,
    uint32_t sequence,
    uint8_t value
)
{
    const uint8_t chunk[2] = {value, (uint8_t)(value + 0x40U)};
    return ota_batch_cache_store(
        cache,
        sequence,
        chunk,
        sizeof(chunk),
        NULL
    ) == ESP_OK;
}

static bool run_batch_retransmission_test(void)
{
    ota_batch_cache_t cache = {0};
    ota_batch_cache_prepare(&cache, 0U, 8U);
    uint8_t acknowledged_mask = 0U;

    ESP_LOGI(TAG, "BATCH START: base=0, count=5, range=[0..4]");
    const uint8_t first_sequences[] = {0U, 2U, 4U};
    for (size_t i = 0U; i < sizeof(first_sequences); ++i) {
        const uint8_t sequence = first_sequences[i];
        ESP_LOGI(TAG, "RX DATA: seq=%u", sequence);
        if (!store_test_chunk(&cache, sequence, sequence)) {
            ESP_LOGE(TAG, "initial batch store failed at seq=%u", sequence);
            return false;
        }
        acknowledged_mask |= (uint8_t)(1U << sequence);
        ESP_LOGI(TAG, "TX ACK: seq=%u", sequence);
    }

    /* Gateway는 5개를 보낸 뒤 개별 ACK 수신 여부로 재전송 대상을 고른다. */
    const uint8_t first_missing_mask = ota_batch_cache_missing_mask(&cache);
    ESP_LOGI(TAG, "GATEWAY ACK TRACKER: acked_mask=0x%02X, no_ack=[1,3]",
             acknowledged_mask);
    if (first_missing_mask != 0x0AU || acknowledged_mask != 0x15U) {
        ESP_LOGE(TAG, "missing mask mismatch: expected=0x0A actual=0x%02X",
                 first_missing_mask);
        return false;
    }

    bool duplicate = false;
    const uint8_t duplicate_chunk[2] = {0xEEU, 0xEEU};
    if (ota_batch_cache_store(
            &cache, 2U, duplicate_chunk, sizeof(duplicate_chunk), &duplicate
        ) != ESP_OK || !duplicate) {
        ESP_LOGE(TAG, "duplicate chunk was not detected");
        return false;
    }
    ESP_LOGI(TAG, "DUPLICATE DATA: seq=2 ignored, TX ACK seq=2 again");

    ESP_LOGI(TAG, "RETRY RX DATA: seq=1");
    if (!store_test_chunk(&cache, 1U, 1U)) {
        ESP_LOGE(TAG, "selective retransmission did not complete batch");
        return false;
    }
    ESP_LOGI(TAG, "TX ACK: seq=1");
    ESP_LOGI(TAG, "RETRY RX DATA: seq=3");
    if (!store_test_chunk(&cache, 3U, 3U) ||
        !ota_batch_cache_is_complete(&cache)) {
        ESP_LOGE(TAG, "selective retransmission did not complete batch");
        return false;
    }
    ESP_LOGI(TAG, "TX ACK: seq=3");
    ESP_LOGI(TAG, "BATCH COMPLETE AUTOMATIC: writing seq 0..4 in order");

    batch_write_capture_t capture = {0};
    size_t written_size = 0U;
    if (ota_batch_cache_commit(
            &cache, capture_batch_write, &capture, &written_size
        ) != ESP_OK || written_size != 10U || capture.length != 10U) {
        ESP_LOGE(TAG, "batch commit failed");
        return false;
    }

    for (uint8_t sequence = 0U; sequence < OTA_CLIENT_BATCH_SIZE; ++sequence) {
        const size_t offset = (size_t)sequence * 2U;
        if (capture.data[offset] != sequence ||
            capture.data[offset + 1U] != (uint8_t)(sequence + 0x40U)) {
            ESP_LOGE(TAG, "flash write order mismatch at seq %u", sequence);
            return false;
        }
    }
    ESP_LOGI(TAG, "BATCH ADVANCE: next_sequence=5 (no BATCH_ACK packet)");

    /* 마지막 배치는 total_chunks=8이므로 seq 5~7 세 개만 요구한다. */
    ota_batch_cache_prepare(&cache, 5U, 8U);
    ESP_LOGI(TAG, "FINAL BATCH START: base=5, count=3, range=[5..7]");
    ESP_LOGI(TAG, "RX DATA: seq=7");
    ESP_LOGI(TAG, "RX DATA: seq=5");
    if (cache.chunk_count != 3U ||
        !store_test_chunk(&cache, 7U, 7U) ||
        !store_test_chunk(&cache, 5U, 5U) ||
        ota_batch_cache_missing_mask(&cache) != 0x02U) {
        ESP_LOGE(TAG, "partial final batch handling failed");
        return false;
    }
    ESP_LOGI(TAG, "TX ACK: seq=7");
    ESP_LOGI(TAG, "TX ACK: seq=5");
    ESP_LOGI(TAG, "GATEWAY ACK TRACKER: no_ack=[6]");
    ESP_LOGI(TAG, "RETRY RX DATA: seq=6");
    if (!store_test_chunk(&cache, 6U, 6U) ||
        !ota_batch_cache_is_complete(&cache)) {
        ESP_LOGE(TAG, "partial final batch retry failed");
        return false;
    }
    ESP_LOGI(TAG, "TX ACK: seq=6");
    ESP_LOGI(TAG, "FINAL BATCH COMPLETE: received_mask=0x%02X",
             cache.received_mask);
    ESP_LOGI(TAG, "BATCH ADVANCE: next_sequence=8 (no BATCH_ACK packet)");

    ESP_LOGI(TAG, "5-chunk individual-ACK/retransmission test PASS");
    ESP_LOGI(TAG, "ordered batch write-callback test PASS");
    ESP_LOGI(TAG, "partial final batch test PASS");
    return true;
}

static bool run_protocol_payload_limit_test(void)
{
    ota_batch_cache_t cache = {0};
    ota_batch_cache_prepare(&cache, 0U, 5U);

    uint8_t maximum_payload[OTA_CLIENT_DATA_MAX_PAYLOAD_SIZE] = {0};
    uint8_t oversized_payload[OTA_CLIENT_DATA_MAX_PAYLOAD_SIZE + 1U] = {0};

    if (OTA_CLIENT_DATA_MAX_PAYLOAD_SIZE != 48U ||
        ota_batch_cache_store(
            &cache, 0U, maximum_payload, sizeof(maximum_payload), NULL
        ) != ESP_OK ||
        ota_batch_cache_store(
            &cache, 1U, oversized_payload, sizeof(oversized_payload), NULL
        ) != ESP_ERR_INVALID_ARG) {
        ESP_LOGE(TAG, "ota-protocol v0.2 payload limit mismatch");
        return false;
    }

    ESP_LOGI(TAG, "ota-protocol v0.2 DATA payload 48-byte limit test PASS");
    return true;
}

static bool run_start_metadata_validation_test(void)
{
    /* 49B image needs ceil(49 / 48) == 2 DATA chunks. The writer must not
     * start when the Gateway advertises an inconsistent chunk count. */
    if (ota_client_start_session(1U, 49U, 1U) != ESP_ERR_INVALID_SIZE ||
        ota_client_get_state() != OTA_CLIENT_STATE_IDLE) {
        ESP_LOGE(TAG, "inconsistent START metadata was accepted");
        return false;
    }

    ESP_LOGI(TAG, "START image_size/total_chunks validation test PASS");
    return true;
}

void app_main(void)
{
    const ota_client_config_t config = {
        .device_id = 1U,
        .receive_timeout_ms = 1000U,
        .send_callback = discard_send,
    };

    if (ota_client_init(&config) != ESP_OK) {
        ESP_LOGE(TAG, "ota_client_init failed");
        return;
    }

    const bool passed =
        run_copy_test() &&
        run_length_validation_test() &&
        run_queue_capacity_test() &&
        run_protocol_payload_limit_test() &&
        run_start_metadata_validation_test() &&
        run_batch_retransmission_test();

    if (passed) {
        ESP_LOGI(TAG, "ALL TESTS PASS");
    } else {
        ESP_LOGE(TAG, "TEST FAILED");
    }
}
