#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
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
        run_queue_capacity_test();

    if (passed) {
        ESP_LOGI(TAG, "ALL TESTS PASS");
    } else {
        ESP_LOGE(TAG, "TEST FAILED");
    }
}
