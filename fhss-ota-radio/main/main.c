#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "fhss_service.h"
#include "fsm.h"

#define FHSS_APP_ROLE_TX 1
#define FHSS_APP_ROLE_RX 2

/* Flash COM3 as TX, then change this to RX and flash COM5. */
#define FHSS_APP_ROLE FHSS_APP_ROLE_RX
#define FHSS_SYNC_TEST_ONLY 0

#define CC1101_SCLK_GPIO GPIO_NUM_12
#define CC1101_MOSI_GPIO GPIO_NUM_11
#define CC1101_MISO_GPIO GPIO_NUM_13
#define CC1101_CS_GPIO   GPIO_NUM_14
#define CC1101_GDO0_GPIO GPIO_NUM_18

static const char *TAG = "main";
static const uint8_t s_hop_channels[] = {0U, 10U, 20U};
static fhss_service_t s_fhss_service;

static void on_fhss_event(fhss_service_event_t event, void *context)
{
    (void)context;

    switch (event) {
    case FHSS_SERVICE_EVENT_SYNC_ACQUIRED:
        ESP_LOGI(TAG, "FHSS service event: SYNC_ACQUIRED");
#if !FHSS_SYNC_TEST_ONLY
        fsm_post_event(FSM_EVENT_SYNC_ACQUIRED);
#endif
        break;
    case FHSS_SERVICE_EVENT_SYNC_LOST:
        ESP_LOGW(TAG, "FHSS service event: SYNC_LOST");
#if !FHSS_SYNC_TEST_ONLY
        fsm_post_event(FSM_EVENT_SYNC_LOST);
#endif
        break;
    case FHSS_SERVICE_EVENT_ERROR:
        ESP_LOGE(TAG, "FHSS service event: ERROR");
#if !FHSS_SYNC_TEST_ONLY
        fsm_post_event(FSM_EVENT_ERROR);
#endif
        break;
    default:
        break;
    }
}

void app_main(void)
{
    const fhss_service_config_t config = {
        .role = FHSS_APP_ROLE == FHSS_APP_ROLE_TX
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
        .slot_duration_us = 300000U,
        .channel_switch_guard_us = 5000U,
        .sync_offset_us = 0U,
        .search_dwell_ms = 137U,
        .receive_timeout_ms = 80U,
        .acquire_count = 3U,
        .loss_count = 5U,
        .diagnostics_interval_ms = 5000U,
        .event_callback = on_fhss_event,
        .event_context = NULL,
    };

#if !FHSS_SYNC_TEST_ONLY
    fsm_init();
    fsm_post_event(FSM_EVENT_INIT_DONE);
#endif

    if (!fhss_service_init(&s_fhss_service, &config)) {
        ESP_LOGE(TAG, "fhss_service_init failed");
#if !FHSS_SYNC_TEST_ONLY
        fsm_post_event(FSM_EVENT_ERROR);
#endif
        return;
    }
    if (!fhss_service_start(&s_fhss_service)) {
        ESP_LOGE(TAG, "fhss_service_start failed");
#if !FHSS_SYNC_TEST_ONLY
        fsm_post_event(FSM_EVENT_ERROR);
#endif
        return;
    }

    ESP_LOGI(TAG, "FHSS service started: role=%s GDO0=GPIO%d",
             config.role == FHSS_SERVICE_ROLE_TX ? "TX" : "RX",
             config.radio.gdo0_gpio);
}
