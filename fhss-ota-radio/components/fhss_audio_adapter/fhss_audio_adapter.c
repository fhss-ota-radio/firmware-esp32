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

/* 재배선(2026-08-14): 앰프(audio_io)와 간섭이 있어 CC1101 핀을 재배정.
 * GPIO14는 이전엔 CS였는데, 보드 하드웨어 배치상 GND와 물리적으로 묶일
 * 수밖에 없는 자리라 이제 어떤 용도로도(CS든 다른 GPIO든) 절대 쓰면 안
 * 된다 — 출력으로 잡고 HIGH를 내보내면 GND와 직결 쇼트난다. CS는 GPIO10으로
 * 옮겨서 회피했다. */
#define CC1101_SCLK_GPIO GPIO_NUM_12
#define CC1101_MOSI_GPIO GPIO_NUM_9
#define CC1101_MISO_GPIO GPIO_NUM_11
#define CC1101_CS_GPIO   GPIO_NUM_10
/* 팀 확정 배선: GDO0(SYNC/RX 타임스탬프 입력)은 GPIO13. 실제 배선과 다르면
 * SYNC edge가 들어오지 않아 RX timeout이 난다. GDO2는 여전히 미사용(코드
 * 어디서도 안 읽음, rf_transport_config_t에 필드조차 없음). */
#define CC1101_GDO0_GPIO GPIO_NUM_13

/* Integration test note (2026-08-15): this board reads CC1101 registers
 * correctly at 10 kHz but returns 0x00 at the previous 1 MHz setting.
 * Start at 100 kHz to retain useful audio throughput while determining the
 * highest reliable SPI clock for the current GPIO9~13 wiring. */
#define CC1101_SPI_CLOCK_HZ 100000

#define FHSS_AUDIO_TX_DRAIN_TIMEOUT_MS 600U

/* 재배정 이력(2026-08-17):
 * 1차 — 기존 {0,10,20}(스모크 테스트 임시값)에서 "일단 최대로" 150개까지
 *   늘림(CC1101 433MHz 로우밴드 387~464MHz 안에서 CHANNR 200kHz 간격 한도).
 * 2차 — 채널 0은 OTA 팀이 라즈베리파이 CC1101 드라이버용으로 예약해서
 *   겹치지 않게 채널 범위를 옮김. 처음엔 랑데부를 맨 끝(150)에 뒀는데
 *   안테나/PA 매칭이 433.92MHz(채널 0) 중심으로 튜닝돼 있어 463.94MHz까지
 *   가니 crc_fail/RADIO_ERROR가 실기기에서 눈에 띄게 늘었음.
 * 3차 — 랑데부를 채널 0과 가장 가까운 1로 옮기고(튜닝 중심에서 거의 안
 *   벗어남), 대역 폭도 150 대신 100까지로 좁혀 안테나 매칭이 나빠지는
 *   구간(위쪽 끝)까지 안 가도록 함. 채널 수는 3->150->100으로, SEARCHING
 *   최악 획득 시간은 이제 무관(랑데부 채널 고정 리슨 방식이라 채널 수와
 *   상관없음 — fhss_service.c rx_task 참고). */
#define FHSS_AUDIO_HOP_CHANNEL_COUNT 100U

static const char *TAG = "fhss_audio_adapter";
static uint8_t s_hop_channels[FHSS_AUDIO_HOP_CHANNEL_COUNT];

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

    /* 채널 0(OTA 팀 예약)은 제외. 랑데부(인덱스 0)는 채널 0과 가장 가까운
     * 1로 둬 안테나/PA 매칭 중심(433.92MHz)에서 거의 안 벗어나게 하고,
     * 나머지 인덱스 1~99에는 2~100을 순서대로 채워 대역을 1~100으로
     * 제한한다(위 파일 상단 주석의 3차 재배정 참고). */
    s_hop_channels[0] = 1U;
    for (size_t i = 1U; i < FHSS_AUDIO_HOP_CHANNEL_COUNT; ++i) {
        s_hop_channels[i] = (uint8_t)(i + 1U);
    }

    const fhss_service_config_t service_config = {
        .role = FHSS_SERVICE_ROLE_RX,
        .radio = {
            .spi_host = SPI2_HOST,
            .sclk_gpio = CC1101_SCLK_GPIO,
            .mosi_gpio = CC1101_MOSI_GPIO,
            .miso_gpio = CC1101_MISO_GPIO,
            .cs_gpio = CC1101_CS_GPIO,
            .gdo0_gpio = CC1101_GDO0_GPIO,
            .spi_clock_hz = CC1101_SPI_CLOCK_HZ,
            .enable_gdo0_interrupt = true,
        },
        .channels = s_hop_channels,
        .channel_count = sizeof(s_hop_channels) / sizeof(s_hop_channels[0]),
        /* Channel 0 belongs to OTA. Both peers use this shared seed to derive
         * the same deterministic audio hopping order. */
        .hop_seed = 0x46485353U,
        .reserved_channel = 0U,
        .slot_duration_us = 300000U,
        .channel_switch_guard_us = 5000U,
        /* 재배정(2026-08-17): 판정 허용 오차를 channel_switch_guard_us(5ms)
         * 재사용에서 분리 — 실제 GDO0 ISR 지연/스케줄링 지터 흡수엔 5ms가
         * 타이트해서, 패킷은 정상 수신됐는데 타이밍만 창을 벗어나 MISS로
         * 판정되는 사례가 있었음(fhss_service.h 주석 참고). */
        .timing_window_margin_us = 20000U,
        .sync_offset_us = 0U,
        /* 재배정(2026-08-17): SEARCHING이 채널 전체를 훑던 시절엔 137ms를
         * 짧게 잡아야 TX 300ms 주기와 위상이 안 맞고(여러 채널을 골고루
         * 훑으려고) 했는데, 지금은 랑데부 채널(0) 하나만 고정으로 듣는다
         * (fhss_service.c rx_task 참고). 이 상태에서 137ms는 오히려 재무장
         * (SIDLE->CHANNR->SFRX->SFTX->RX 재시작) 횟수만 잦아지고, 그 짧은
         * 재무장 공백과 TX의 랑데부 SYNC 송신 순간이 겹치면 통째로 놓치는
         * 사례가 실기기에서 확인됨("송신해도 수신자가 RX로 안 들어감").
         * 재무장 빈도를 줄여 공백 노출을 줄이려고 400ms로 상향 — PTT 응답
         * 지연도 이 값만큼 늘어날 수 있어(최악 SEARCHING 중 PTT 누른 경우)
         * 너무 크게는 안 올림. */
        .search_dwell_ms = 400U,
        .receive_timeout_ms = 80U,
        .acquire_count = 3U,
        .loss_count = 5U,
        .recovery_entry_miss_count = 2U,
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
