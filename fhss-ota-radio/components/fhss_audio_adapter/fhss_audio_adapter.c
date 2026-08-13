#include "fhss_audio_adapter.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "fhss_audio_packet.h"
#include "fhss_service.h"
#include "audio_codec.h"

#define CC1101_SCLK_GPIO GPIO_NUM_12
#define CC1101_MOSI_GPIO GPIO_NUM_11
#define CC1101_MISO_GPIO GPIO_NUM_13
#define CC1101_CS_GPIO   GPIO_NUM_14
#define CC1101_GDO0_GPIO GPIO_NUM_9

#define FHSS_AUDIO_TX_DRAIN_TIMEOUT_MS 600U

static const char *TAG = "fhss_audio_adapter";
static const uint8_t s_hop_channels[] = {0U, 10U, 20U};

typedef struct {
    fhss_service_t service;
    fhss_audio_adapter_config_t config;
    uint8_t frames[FHSS_AUDIO_PACKET_MAX_FRAMES][AUDIO_CODEC_MAX_ENCODED_BYTES];
    size_t frame_lengths[FHSS_AUDIO_PACKET_MAX_FRAMES];
    size_t frame_count;
    uint16_t tx_sequence;
    uint16_t expected_rx_sequence;
    uint32_t tx_packet_count;
    uint32_t rx_packet_count;
    bool have_rx_sequence;
    bool initialized;
    bool tx_active;
} fhss_audio_adapter_state_t;

static fhss_audio_adapter_state_t s_adapter;

static void on_service_event(fhss_service_event_t event, void *context)
{
    (void)context;
    switch (event) {
    case FHSS_SERVICE_EVENT_SYNC_ACQUIRED:
        ESP_LOGI(TAG, "SYNC_ACQUIRED");
        break;
    case FHSS_SERVICE_EVENT_SYNC_LOST:
        ESP_LOGW(TAG, "SYNC_LOST");
        if (s_adapter.config.event_callback != NULL) {
            s_adapter.config.event_callback(
                FHSS_AUDIO_ADAPTER_EVENT_SYNC_LOST,
                s_adapter.config.callback_context);
        }
        break;
    case FHSS_SERVICE_EVENT_ERROR:
        ESP_LOGE(TAG, "SERVICE_ERROR");
        if (s_adapter.config.event_callback != NULL) {
            s_adapter.config.event_callback(
                FHSS_AUDIO_ADAPTER_EVENT_ERROR,
                s_adapter.config.callback_context);
        }
        break;
    default:
        break;
    }
}

static void on_service_data(
    const uint8_t *data,
    size_t length,
    void *context
)
{
    (void)context;
    fhss_audio_packet_view_t packet = {0};
    if (fhss_audio_packet_unpack(data, length, &packet) !=
        FHSS_AUDIO_PACKET_STATUS_OK) {
        ESP_LOGW(TAG, "dropping invalid audio packet: length=%u",
                 (unsigned)length);
        return;
    }

    if (s_adapter.have_rx_sequence &&
        packet.sequence != s_adapter.expected_rx_sequence) {
        ESP_LOGW(TAG, "audio packet gap: expected=%u received=%u",
                 s_adapter.expected_rx_sequence, packet.sequence);
    }
    s_adapter.expected_rx_sequence = (uint16_t)(packet.sequence + 1U);
    s_adapter.have_rx_sequence = true;
    s_adapter.rx_packet_count++;

    if ((s_adapter.rx_packet_count % 25U) == 0U) {
        ESP_LOGI(TAG,
                 "AUDIO_RX packet=%lu sequence=%u frames=%u bytes=%u flags=0x%02X",
                 (unsigned long)s_adapter.rx_packet_count,
                 packet.sequence,
                 (unsigned)packet.frame_count,
                 (unsigned)length,
                 packet.flags);
    }

    for (size_t i = 0U; i < packet.frame_count; ++i) {
        if (s_adapter.config.rx_frame_callback == NULL ||
            !s_adapter.config.rx_frame_callback(
                packet.frames[i].data,
                packet.frames[i].length,
                s_adapter.config.callback_context)) {
            ESP_LOGW(TAG, "RX audio frame dropped: packet=%u frame=%u",
                     packet.sequence, (unsigned)i);
        }
    }
}

static bool send_buffered_frames(uint8_t flags)
{
    if (s_adapter.frame_count == 0U) {
        return true;
    }
    fhss_audio_frame_view_t frames[FHSS_AUDIO_PACKET_MAX_FRAMES] = {0};
    for (size_t i = 0U; i < s_adapter.frame_count; ++i) {
        frames[i].data = s_adapter.frames[i];
        frames[i].length = s_adapter.frame_lengths[i];
    }

    uint8_t packet[RF_TRANSPORT_MAX_PACKET_LENGTH] = {0};
    size_t packet_length = 0U;
    const fhss_audio_packet_status_t status = fhss_audio_packet_pack(
        s_adapter.tx_sequence,
        flags,
        frames,
        s_adapter.frame_count,
        packet,
        sizeof(packet),
        &packet_length);
    if (status != FHSS_AUDIO_PACKET_STATUS_OK) {
        ESP_LOGE(TAG, "audio packet pack failed: status=%d", status);
        return false;
    }
    if (!fhss_service_send_data(&s_adapter.service, packet, packet_length)) {
        ESP_LOGW(TAG, "audio TX queue full: sequence=%u", s_adapter.tx_sequence);
        return false;
    }
    s_adapter.tx_packet_count++;
    if ((s_adapter.tx_packet_count % 25U) == 0U) {
        ESP_LOGI(TAG,
                 "AUDIO_TX packet=%lu sequence=%u frames=%u bytes=%u flags=0x%02X",
                 (unsigned long)s_adapter.tx_packet_count,
                 s_adapter.tx_sequence,
                 (unsigned)s_adapter.frame_count,
                 (unsigned)packet_length,
                 flags);
    }
    s_adapter.tx_sequence++;
    s_adapter.frame_count = 0U;
    memset(s_adapter.frame_lengths, 0, sizeof(s_adapter.frame_lengths));
    return true;
}

bool fhss_audio_adapter_init(const fhss_audio_adapter_config_t *config)
{
    if (config == NULL || config->rx_frame_callback == NULL) {
        return false;
    }
    memset(&s_adapter, 0, sizeof(s_adapter));
    s_adapter.config = *config;

    const fhss_service_config_t service_config = {
        .role = FHSS_SERVICE_ROLE_RX,
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
        .event_callback = on_service_event,
        .data_callback = on_service_data,
        .event_context = NULL,
    };
    if (!fhss_service_init(&s_adapter.service, &service_config) ||
        !fhss_service_start(&s_adapter.service)) {
        ESP_LOGE(TAG, "FHSS service initialization failed");
        return false;
    }
    s_adapter.initialized = true;
    ESP_LOGI(TAG, "ready: RX standby, GDO0=GPIO%d", CC1101_GDO0_GPIO);
    return true;
}

bool fhss_audio_adapter_begin_tx(void)
{
    if (!s_adapter.initialized || s_adapter.tx_active) {
        return false;
    }
    s_adapter.frame_count = 0U;
    s_adapter.tx_sequence = 0U;
    s_adapter.tx_packet_count = 0U;
    if (!fhss_service_set_role(&s_adapter.service, FHSS_SERVICE_ROLE_TX)) {
        return false;
    }
    s_adapter.tx_active = true;
    ESP_LOGI(TAG, "TX session started");
    return true;
}

bool fhss_audio_adapter_submit_encoded_frame(
    const uint8_t *frame,
    size_t length
)
{
    if (!s_adapter.tx_active || frame == NULL || length == 0U ||
        length > AUDIO_CODEC_MAX_ENCODED_BYTES ||
        s_adapter.frame_count >= FHSS_AUDIO_PACKET_MAX_FRAMES) {
        return false;
    }
    memcpy(s_adapter.frames[s_adapter.frame_count], frame, length);
    s_adapter.frame_lengths[s_adapter.frame_count] = length;
    s_adapter.frame_count++;
    return s_adapter.frame_count < FHSS_AUDIO_PACKET_MAX_FRAMES ||
           send_buffered_frames(0U);
}

bool fhss_audio_adapter_end_tx(void)
{
    if (!s_adapter.initialized || !s_adapter.tx_active) {
        return true;
    }
    bool ok = send_buffered_frames(FHSS_AUDIO_PACKET_FLAG_END_OF_TALKSPURT);
    /* A short PTT press can end before the first 300 ms FHSS slot starts.
     * Wait for both the software queue and the CC1101 transaction instead of
     * using a fixed delay, otherwise the final talkspurt packet can be lost. */
    if (!fhss_service_wait_tx_idle(
            &s_adapter.service, FHSS_AUDIO_TX_DRAIN_TIMEOUT_MS)) {
        ESP_LOGW(TAG, "timed out while draining final audio packet");
        ok = false;
    }
    if (!fhss_service_set_role(&s_adapter.service, FHSS_SERVICE_ROLE_RX)) {
        ok = false;
    }
    s_adapter.tx_active = false;
    s_adapter.have_rx_sequence = false;
    ESP_LOGI(TAG, "TX session ended: packets=%lu; RX standby resumed",
             (unsigned long)s_adapter.tx_packet_count);
    return ok;
}
