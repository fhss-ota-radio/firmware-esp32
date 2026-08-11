#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "fhss_hop_sequence.h"
#include "fsm.h"
#include "rf_transport.h"

#define CC1101_SMOKE_SCLK_GPIO GPIO_NUM_12
#define CC1101_SMOKE_MOSI_GPIO GPIO_NUM_11
#define CC1101_SMOKE_MISO_GPIO GPIO_NUM_13
#define CC1101_SMOKE_CS_GPIO   GPIO_NUM_14
#define CC1101_SMOKE_SPI_HZ    1000000
#define CC1101_SMOKE_TEST_ONLY  1

#define CC1101_TEST_ROLE_TX 1
#define CC1101_TEST_ROLE_RX 2

/* Flash one board as TX, then change this to RX and flash the other board. */
#define CC1101_TEST_ROLE CC1101_TEST_ROLE_RX

#define CC1101_FHSS_TEST_ENABLED       1
#define CC1101_FHSS_TX_REPEAT_COUNT    3U
#define CC1101_FHSS_TX_INTERVAL_MS     100U
#define CC1101_FHSS_RX_DWELL_MS        137U

static const uint8_t s_fhss_channels[] = {0U, 10U, 20U};

static const char *TAG = "cc1101_smoke";
static rf_transport_t s_transport;

static void cc1101_smoke_test(void)
{
    const rf_transport_config_t config = {
        .spi_host = SPI2_HOST,
        .sclk_gpio = CC1101_SMOKE_SCLK_GPIO,
        .mosi_gpio = CC1101_SMOKE_MOSI_GPIO,
        .miso_gpio = CC1101_SMOKE_MISO_GPIO,
        .cs_gpio = CC1101_SMOKE_CS_GPIO,
        .spi_clock_hz = CC1101_SMOKE_SPI_HZ,
    };

    ESP_LOGI(TAG,
             "Starting CC1101 SPI smoke test (SCLK=%d, MOSI=%d, MISO=%d, CS=%d)",
             config.sclk_gpio,
             config.mosi_gpio,
             config.miso_gpio,
             config.cs_gpio);

    const rf_transport_status_t init_status =
        rf_transport_init(&s_transport, &config);
    if (init_status != RF_TRANSPORT_STATUS_OK) {
        ESP_LOGE(TAG, "rf_transport_init failed: status=%d", init_status);
        return;
    }

    rf_transport_chip_info_t chip_info = {0};
    const rf_transport_status_t read_status =
        rf_transport_read_chip_info(&s_transport, &chip_info);
    if (read_status != RF_TRANSPORT_STATUS_OK) {
        ESP_LOGE(TAG, "CC1101 chip-info read failed: status=%d", read_status);
        return;
    }

    ESP_LOGI(TAG,
             "CC1101 SPI OK: PARTNUM=0x%02X, VERSION=0x%02X",
             chip_info.partnum,
             chip_info.version);
}

#if !CC1101_FHSS_TEST_ENABLED
static void cc1101_radio_test(void)
{
    if (!s_transport.initialized) {
        ESP_LOGE(TAG, "Radio test skipped because SPI initialization failed");
        return;
    }

    const rf_transport_status_t configure_status =
        rf_transport_configure_433mhz(&s_transport);
    if (configure_status != RF_TRANSPORT_STATUS_OK) {
        ESP_LOGE(TAG, "433 MHz radio configuration failed: status=%d", configure_status);
        return;
    }

    if (CC1101_TEST_ROLE == CC1101_TEST_ROLE_TX) {
        uint32_t sequence = 0U;
        ESP_LOGI(TAG, "433.92 MHz radio test: TX mode, -30 dBm, 1 packet/second");

        for (;;) {
            char payload[RF_TRANSPORT_MAX_PACKET_LENGTH + 1U] = {0};
            const int length = snprintf(payload, sizeof(payload), "PING:%lu",
                                        (unsigned long)sequence);
            if (length <= 0 || length > (int)RF_TRANSPORT_MAX_PACKET_LENGTH) {
                ESP_LOGE(TAG, "Failed to create TX payload");
                return;
            }

            const rf_transport_status_t send_status =
                rf_transport_send_packet(
                    &s_transport,
                    (const uint8_t *)payload,
                    (uint8_t)length
                );

            if (send_status == RF_TRANSPORT_STATUS_OK) {
                ESP_LOGI(TAG, "TX PASS: seq=%lu payload=\"%s\"",
                         (unsigned long)sequence, payload);
                sequence++;
            } else {
                ESP_LOGE(TAG, "TX FAIL: seq=%lu status=%d",
                         (unsigned long)sequence, send_status);
            }

            vTaskDelay(pdMS_TO_TICKS(1000U));
        }
    }

    ESP_LOGI(TAG, "433.92 MHz radio test: RX mode, waiting for PING packets");
    for (;;) {
        rf_transport_rx_packet_t packet = {0};
        const rf_transport_status_t receive_status =
            rf_transport_receive_packet(&s_transport, 1000U, &packet);

        if (receive_status == RF_TRANSPORT_STATUS_TIMEOUT) {
            continue;
        }
        if (receive_status != RF_TRANSPORT_STATUS_OK) {
            ESP_LOGE(TAG, "RX FAIL: status=%d", receive_status);
            vTaskDelay(pdMS_TO_TICKS(100U));
            continue;
        }

        char payload[RF_TRANSPORT_MAX_PACKET_LENGTH + 1U] = {0};
        memcpy(payload, packet.payload, packet.length);

        ESP_LOGI(TAG,
                 "RX %s: payload=\"%s\" length=%u RSSI=%d dBm LQI=%u",
                 packet.crc_ok ? "PASS" : "CRC_FAIL",
                 payload,
                 packet.length,
                 packet.rssi_dbm,
                 packet.lqi);
    }
}
#endif

static bool cc1101_select_hop_channel(
    const fhss_hop_sequence_t *sequence,
    uint32_t slot,
    uint8_t *out_channel
)
{
    const fhss_hop_status_t hop_status =
        fhss_hop_sequence_get_channel(sequence, slot, out_channel);
    if (hop_status != FHSS_HOP_STATUS_OK) {
        ESP_LOGE(TAG, "Hop calculation failed: slot=%lu status=%d",
                 (unsigned long)slot, hop_status);
        return false;
    }

    const rf_transport_status_t channel_status =
        rf_transport_set_channel(&s_transport, *out_channel);
    if (channel_status != RF_TRANSPORT_STATUS_OK) {
        ESP_LOGE(TAG, "Channel change failed: slot=%lu channel=%u status=%d",
                 (unsigned long)slot, *out_channel, channel_status);
        return false;
    }
    return true;
}

static void cc1101_fhss_test(void)
{
    if (!s_transport.initialized) {
        ESP_LOGE(TAG, "FHSS test skipped because SPI initialization failed");
        return;
    }

    rf_transport_status_t status = rf_transport_configure_433mhz(&s_transport);
    if (status != RF_TRANSPORT_STATUS_OK) {
        ESP_LOGE(TAG, "433 MHz radio configuration failed: status=%d", status);
        return;
    }

    fhss_hop_sequence_t sequence = {0};
    const fhss_hop_status_t init_status = fhss_hop_sequence_init(
        &sequence,
        s_fhss_channels,
        sizeof(s_fhss_channels) / sizeof(s_fhss_channels[0])
    );
    if (init_status != FHSS_HOP_STATUS_OK) {
        ESP_LOGE(TAG, "Hop sequence initialization failed: status=%d", init_status);
        return;
    }

    uint32_t slot = 0U;
    if (CC1101_TEST_ROLE == CC1101_TEST_ROLE_TX) {
        ESP_LOGI(TAG, "FHSS TX: channels={0,10,20}, three packets per hop");
        for (;;) {
            uint8_t channel = 0U;
            if (!cc1101_select_hop_channel(&sequence, slot, &channel)) {
                vTaskDelay(pdMS_TO_TICKS(500U));
                continue;
            }

            for (uint32_t repeat = 0U; repeat < CC1101_FHSS_TX_REPEAT_COUNT; ++repeat) {
                char payload[RF_TRANSPORT_MAX_PACKET_LENGTH + 1U] = {0};
                const int length = snprintf(payload, sizeof(payload),
                                            "FHSS:%lu:%u:%lu",
                                            (unsigned long)slot,
                                            channel,
                                            (unsigned long)repeat);
                status = rf_transport_send_packet(
                    &s_transport, (const uint8_t *)payload, (uint8_t)length);
                if (status == RF_TRANSPORT_STATUS_OK) {
                    ESP_LOGI(TAG, "FHSS TX PASS: slot=%lu channel=%u repeat=%lu",
                             (unsigned long)slot, channel, (unsigned long)repeat);
                } else {
                    ESP_LOGE(TAG, "FHSS TX FAIL: slot=%lu channel=%u status=%d",
                             (unsigned long)slot, channel, status);
                }
                vTaskDelay(pdMS_TO_TICKS(CC1101_FHSS_TX_INTERVAL_MS));
            }
            slot++;
        }
    }

    ESP_LOGI(TAG, "FHSS RX: scanning channels={0,10,20}");
    for (;;) {
        uint8_t channel = 0U;
        if (!cc1101_select_hop_channel(&sequence, slot, &channel)) {
            vTaskDelay(pdMS_TO_TICKS(500U));
            continue;
        }

        rf_transport_rx_packet_t packet = {0};
        status = rf_transport_receive_packet(
            &s_transport, CC1101_FHSS_RX_DWELL_MS, &packet);
        if (status == RF_TRANSPORT_STATUS_OK) {
            char payload[RF_TRANSPORT_MAX_PACKET_LENGTH + 1U] = {0};
            memcpy(payload, packet.payload, packet.length);
            ESP_LOGI(TAG,
                     "FHSS RX %s: scan_slot=%lu channel=%u payload=\"%s\" RSSI=%d dBm LQI=%u",
                     packet.crc_ok ? "PASS" : "CRC_FAIL",
                     (unsigned long)slot,
                     channel,
                     payload,
                     packet.rssi_dbm,
                     packet.lqi);
        } else if (status != RF_TRANSPORT_STATUS_TIMEOUT) {
            ESP_LOGE(TAG, "FHSS RX FAIL: scan_slot=%lu channel=%u status=%d",
                     (unsigned long)slot, channel, status);
        }
        slot++;
    }
}

void app_main(void)
{
    cc1101_smoke_test();

#if CC1101_SMOKE_TEST_ONLY
#if CC1101_FHSS_TEST_ENABLED
    cc1101_fhss_test();
#else
    cc1101_radio_test();
#endif
#else
    fsm_init();

    /* TODO: 실제 주변장치 초기화(I2S/OLED/SPI/GPIO)가 끝난 뒤 아래 이벤트를 발생시킨다. */
    fsm_post_event(FSM_EVENT_INIT_DONE);
#endif
}
