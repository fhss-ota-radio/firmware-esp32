#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "fhss_service.h"

#define FHSS_TEST_ROLE_TX 1
#define FHSS_TEST_ROLE_RX 2

/* Flash COM3 as TX, then change this to RX and flash COM5. */
#define FHSS_TEST_ROLE FHSS_TEST_ROLE_RX

#define CC1101_SCLK_GPIO GPIO_NUM_12
#define CC1101_MOSI_GPIO GPIO_NUM_11
#define CC1101_MISO_GPIO GPIO_NUM_13
#define CC1101_CS_GPIO   GPIO_NUM_14
#define CC1101_GDO0_GPIO GPIO_NUM_9

static const char *TAG = "fhss_sync_test";
/* Channel 0 is reserved for OTA; channel 1 is the audio rendezvous channel. */
static const uint8_t s_hop_channels[] = {1U, 10U, 20U};
static fhss_service_t s_fhss_service;

static void on_fhss_event(fhss_service_event_t event, void *context)
{
    (void)context;

    switch (event) {
    case FHSS_SERVICE_EVENT_SYNC_ACQUIRED:
        ESP_LOGI(TAG, "SYNC_ACQUIRED");
        break;
    case FHSS_SERVICE_EVENT_SYNC_LOST:
        ESP_LOGW(TAG, "SYNC_LOST");
        break;
    case FHSS_SERVICE_EVENT_ERROR:
        ESP_LOGE(TAG, "SERVICE_ERROR");
        break;
    default:
        break;
    }
}

void app_main(void)
{
    const fhss_service_config_t config = {
        .role = FHSS_TEST_ROLE == FHSS_TEST_ROLE_TX
            ? FHSS_SERVICE_ROLE_TX
            : FHSS_SERVICE_ROLE_RX,
        .radio = {
            .spi_host = SPI2_HOST,
            .sclk_gpio = CC1101_SCLK_GPIO,
            .mosi_gpio = CC1101_MOSI_GPIO,
            .miso_gpio = CC1101_MISO_GPIO,
            .cs_gpio = CC1101_CS_GPIO,
            .gdo0_gpio = CC1101_GDO0_GPIO,
            .spi_clock_hz = 1000000,
            .enable_gdo0_interrupt = true,
        },
        .channels = s_hop_channels,
        .channel_count = sizeof(s_hop_channels) / sizeof(s_hop_channels[0]),
        .hop_seed = 0x46485353U,
        .generation = 0U,
        .reserved_channel = 0U,
        .slot_duration_us = 300000U,
        .channel_switch_guard_us = 5000U,
        .sync_offset_us = 0U,
        .correction_deadband_us = 500U,
        .correction_fast_threshold_us = 2000U,
        .correction_slow_divisor = 8U,
        .correction_fast_divisor = 2U,
        .search_dwell_ms = 137U,
        .receive_timeout_ms = 80U,
        .acquire_count = 3U,
        .loss_count = 5U,
        .recovery_entry_miss_count = 2U,
        .diagnostics_interval_ms = 5000U,
        .event_callback = on_fhss_event,
        .event_context = NULL,
    };

    if (!fhss_service_init(&s_fhss_service, &config)) {
        ESP_LOGE(TAG, "fhss_service_init failed");
        return;
    }
    if (!fhss_service_start(&s_fhss_service)) {
        ESP_LOGE(TAG, "fhss_service_start failed");
        return;
    }

    ESP_LOGI(TAG, "started: role=%s GDO0=GPIO%d",
             config.role == FHSS_SERVICE_ROLE_TX ? "TX" : "RX",
             config.radio.gdo0_gpio);
}

