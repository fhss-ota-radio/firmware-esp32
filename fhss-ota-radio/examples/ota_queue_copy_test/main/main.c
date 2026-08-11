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
    ESP_LOGI(TAG, "FLASH WRITE: seq=%u, length=%u",
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

    ESP_LOGI(TAG, "BATCH START: base=0, count=5, range=[0..4]");
    ESP_LOGI(TAG, "RX DATA: seq=0");
    ESP_LOGI(TAG, "RX DATA: seq=2");
    ESP_LOGI(TAG, "RX DATA: seq=4");

    if (!store_test_chunk(&cache, 0U, 0U) ||
        !store_test_chunk(&cache, 2U, 2U) ||
        !store_test_chunk(&cache, 4U, 4U)) {
        ESP_LOGE(TAG, "initial batch store failed");
        return false;
    }

    /* seq 1과 3만 빠졌으므로 bit 1, 3이 설정되어야 한다. */
    const uint8_t first_missing_mask = ota_batch_cache_missing_mask(&cache);
    ESP_LOGI(TAG, "BATCH CHECK: base=0, received_mask=0x%02X",
             cache.received_mask);
    ESP_LOGI(TAG, "BATCH NACK: missing_mask=0x%02X, missing=[1,3]",
             first_missing_mask);
    if (first_missing_mask != 0x0AU) {
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
    ESP_LOGI(TAG, "DUPLICATE DATA: seq=2 ignored");

    ESP_LOGI(TAG, "RETRY RX DATA: seq=1");
    ESP_LOGI(TAG, "RETRY RX DATA: seq=3");
    if (!store_test_chunk(&cache, 1U, 1U) ||
        !store_test_chunk(&cache, 3U, 3U) ||
        !ota_batch_cache_is_complete(&cache)) {
        ESP_LOGE(TAG, "selective retransmission did not complete batch");
        return false;
    }
    ESP_LOGI(TAG, "BATCH CHECK: base=0, received_mask=0x%02X, missing_mask=0x00",
             cache.received_mask);
    ESP_LOGI(TAG, "BATCH COMPLETE: writing seq 0..4 in order");

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
    ESP_LOGI(TAG, "BATCH ACK: base=0, next_sequence=5");

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
    ESP_LOGI(TAG, "BATCH NACK: base=5, missing_mask=0x02, missing=[6]");
    ESP_LOGI(TAG, "RETRY RX DATA: seq=6");
    if (!store_test_chunk(&cache, 6U, 6U) ||
        !ota_batch_cache_is_complete(&cache)) {
        ESP_LOGE(TAG, "partial final batch retry failed");
        return false;
    }
    ESP_LOGI(TAG, "FINAL BATCH COMPLETE: received_mask=0x%02X",
             cache.received_mask);
    ESP_LOGI(TAG, "BATCH ACK: base=5, next_sequence=8");

    ESP_LOGI(TAG, "5-chunk missing-mask/retransmission test PASS");
    ESP_LOGI(TAG, "ordered batch flash-write test PASS");
    ESP_LOGI(TAG, "partial final batch test PASS");
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
        run_batch_retransmission_test();

    if (passed) {
        ESP_LOGI(TAG, "ALL TESTS PASS");
    } else {
        ESP_LOGE(TAG, "TEST FAILED");
    }
}
