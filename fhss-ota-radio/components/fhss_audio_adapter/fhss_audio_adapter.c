#include "fhss_audio_adapter.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "fhss_audio_packet.h"
#include "fhss_config_store.h"
#include "fhss_ota_diagnostics.h"
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

/*
 * [2026-08-25 추가] 연속 몇 번 SYNC를 놓치면 "완전히 잃었다"고 판정할지의
 * 임계값 — 음성용과 OTA용을 분리한다.
 *
 * [왜 분리하나] 원래는 하나뿐이었고 값은 음성 기준(5/2)이었다. 음성은
 * 이쪽이 송신하는 시간이 PTT를 누른 동안뿐이라 자기 SYNC 수신을 방해할
 * 일이 거의 없어서 이 정도로 충분하다. 오히려 짧아야 좋다 — 상대가
 * 범위를 벗어나면 빨리 판정하고 고정 채널로 돌아와야 다음 송신을 놓치지
 * 않기 때문이다.
 *
 * 그런데 OTA 전송 중에는 상황이 정반대다. DATA를 받을 때마다 곧바로
 * ACK를 송신하는데 CC1101은 반이중(half-duplex)이라 그 송신 시간(실측
 * 121ms) 동안 귀가 닫힌다. 슬롯이 300ms이므로 ACK 송신 타이밍이 슬롯
 * 경계(=SYNC 도착 시점)와 겹치면 그 슬롯의 SYNC를 통째로 놓치고,
 * 트래픽이 몰리면 연속으로 발생한다. 5회(=1.5초)면 너무 쉽게 하드
 * 로스로 넘어간다.
 *
 * 하드 로스가 나면 스케줄러 기준점을 버리고 랑데부 채널로 돌아가
 * 재획득에 최악 3.3초가 걸리는데, 그 사이 Gateway 재전송이 다시 획득을
 * 방해해서 세션째로 날아간다(148 실기기 2026-08-24, 1036청크).
 *
 * 그래서 OTA에서만 12회(=3.6초)로 완화하고, OTA를 벗어나면 음성용 값으로
 * 되돌린다. 두 값 모두 reset_controller()가 다시 읽어가는데, 이 함수는
 * fhss_service_set_role() 안에서 호출되고 OTA 진입(activate_ota_fhss)과
 * 종료(end_ota) 둘 다 set_role을 거치므로 전환 시점이 보장된다.
 *
 * 제약: recovery_entry_miss_count < loss_count (fhss_service.c 961행).
 * 상세: gateway-ota/docs/note/design-notes-gateway-ota-es.md 58절.
 */
#define FHSS_SYNC_LOSS_COUNT_VOICE      5U
#define FHSS_RECOVERY_ENTRY_MISS_VOICE  2U
#define FHSS_SYNC_LOSS_COUNT_OTA        12U
#define FHSS_RECOVERY_ENTRY_MISS_OTA    4U
#define FHSS_AUDIO_END_REPEAT_COUNT 3U
#define OTA_FIXED_CHANNEL 0U

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
#define FHSS_OTA_RX_QUEUE_DEPTH 16U

typedef struct {
    uint8_t data[RF_TRANSPORT_MAX_PACKET_LENGTH];
    uint8_t length;
} fhss_ota_rx_item_t;

static const char *TAG = "fhss_audio_adapter";
static uint8_t s_hop_channels[FHSS_AUDIO_HOP_CHANNEL_COUNT];

typedef struct {
    fhss_service_t service;
    fhss_audio_adapter_config_t config;
    uint8_t frames[FHSS_AUDIO_PACKET_MAX_FRAMES][AUDIO_CODEC_MAX_ENCODED_BYTES];
    size_t frame_lengths[FHSS_AUDIO_PACKET_MAX_FRAMES];
    size_t frame_count;
    uint16_t tx_sequence;
    uint16_t tx_session_id;
    uint16_t expected_rx_sequence;
    uint32_t tx_packet_count;
    uint32_t rx_packet_count;
    bool have_rx_sequence;
    bool have_last_rx_end;
    uint16_t last_rx_end_session_id;
    uint16_t last_rx_end_sequence;
    bool initialized;
    bool tx_active;
    bool ota_active;
    bool ota_fhss_active;
    QueueHandle_t ota_rx_queue;
    SemaphoreHandle_t radio_mutex;
} fhss_audio_adapter_state_t;

static fhss_audio_adapter_state_t s_adapter;

static void on_service_event(fhss_service_event_t event, void *context)
{
    (void)context;
    switch (event) {
    case FHSS_SERVICE_EVENT_SYNC_ACQUIRED:
        ESP_LOGI(TAG, "SYNC_ACQUIRED");
        if (s_adapter.config.event_callback != NULL) {
            s_adapter.config.event_callback(
                FHSS_AUDIO_ADAPTER_EVENT_SYNC_ACQUIRED,
                s_adapter.config.callback_context);
        }
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

static fhss_service_data_action_t on_service_data(
    const uint8_t *data,
    size_t length,
    void *context
)
{
    (void)context;
    if (s_adapter.ota_fhss_active) {
        if (length > 0U && length <= RF_TRANSPORT_MAX_PACKET_LENGTH &&
            s_adapter.ota_rx_queue != NULL) {
            fhss_ota_rx_item_t item = { .length = (uint8_t)length };
            memcpy(item.data, data, length);
            (void)xQueueSend(s_adapter.ota_rx_queue, &item, 0U);
        }
        return FHSS_SERVICE_DATA_CONTINUE;
    }
    fhss_audio_end_packet_t end = {0};
    if (fhss_audio_end_packet_unpack(data, length, &end) ==
        FHSS_AUDIO_PACKET_STATUS_OK) {
        const bool duplicate = s_adapter.have_last_rx_end &&
            end.session_id == s_adapter.last_rx_end_session_id &&
            end.final_sequence == s_adapter.last_rx_end_sequence;
        if (!duplicate) {
            s_adapter.have_last_rx_end = true;
            s_adapter.last_rx_end_session_id = end.session_id;
            s_adapter.last_rx_end_sequence = end.final_sequence;
            s_adapter.have_rx_sequence = false;
            ESP_LOGI(TAG, "TALKSPURT_END RX: session=%u final_sequence=%u",
                     end.session_id, end.final_sequence);
            if (s_adapter.config.event_callback != NULL) {
                s_adapter.config.event_callback(
                    FHSS_AUDIO_ADAPTER_EVENT_TALKSPURT_ENDED,
                    s_adapter.config.callback_context);
            }
        }
        /* Repeated END packets must also return the radio to rendezvous mode,
         * but only the first copy is forwarded to the application FSM. */
        return FHSS_SERVICE_DATA_SESSION_END;
    }

    fhss_audio_packet_view_t packet = {0};
    if (fhss_audio_packet_unpack(data, length, &packet) !=
        FHSS_AUDIO_PACKET_STATUS_OK) {
        ESP_LOGW(TAG, "dropping invalid audio packet: length=%u",
                 (unsigned)length);
        return FHSS_SERVICE_DATA_CONTINUE;
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
    return FHSS_SERVICE_DATA_CONTINUE;
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

static bool send_end_packets(void)
{
    const fhss_audio_end_packet_t end = {
        .session_id = s_adapter.tx_session_id,
        .final_sequence = s_adapter.tx_sequence == 0U
            ? FHSS_AUDIO_END_NO_AUDIO_SEQUENCE
            : (uint16_t)(s_adapter.tx_sequence - 1U),
        .reason = FHSS_AUDIO_END_REASON_PTT_RELEASE,
    };
    uint8_t packet[FHSS_AUDIO_END_PACKET_SIZE] = {0};
    size_t packet_length = 0U;
    if (fhss_audio_end_packet_pack(
            &end, packet, sizeof(packet), &packet_length) !=
        FHSS_AUDIO_PACKET_STATUS_OK) {
        return false;
    }
    for (uint32_t i = 0U; i < FHSS_AUDIO_END_REPEAT_COUNT; ++i) {
        if (!fhss_service_send_data(
                &s_adapter.service, packet, packet_length)) {
            ESP_LOGW(TAG, "END TX queue full: copy=%lu",
                     (unsigned long)(i + 1U));
            return false;
        }
    }
    ESP_LOGI(TAG, "TALKSPURT_END TX: session=%u final_sequence=%u copies=%u",
             end.session_id, end.final_sequence, FHSS_AUDIO_END_REPEAT_COUNT);
    return true;
}

bool fhss_audio_adapter_init(const fhss_audio_adapter_config_t *config)
{
    if (config == NULL || config->rx_frame_callback == NULL) {
        return false;
    }
    memset(&s_adapter, 0, sizeof(s_adapter));
    s_adapter.config = *config;
    s_adapter.radio_mutex = xSemaphoreCreateMutex();
    s_adapter.ota_rx_queue = xQueueCreate(
        FHSS_OTA_RX_QUEUE_DEPTH, sizeof(fhss_ota_rx_item_t));
    if (s_adapter.radio_mutex == NULL || s_adapter.ota_rx_queue == NULL) {
        return false;
    }

    ota_fhss_config_fields_t active_config = {0};
    const bool have_active_config =
        fhss_config_store_init() == ESP_OK &&
        fhss_config_store_load_active(&active_config) == ESP_OK &&
        active_config.channel_count <= FHSS_AUDIO_HOP_CHANNEL_COUNT;
    const size_t configured_channel_count = have_active_config
        ? active_config.channel_count
        : FHSS_AUDIO_HOP_CHANNEL_COUNT;

    /* 채널 0(OTA 팀 예약)은 제외. 랑데부(인덱스 0)는 채널 0과 가장 가까운
     * 1로 둬 안테나/PA 매칭 중심(433.92MHz)에서 거의 안 벗어나게 하고,
     * 나머지 인덱스 1~99에는 2~100을 순서대로 채워 대역을 1~100으로
     * 제한한다(위 파일 상단 주석의 3차 재배정 참고). */
    for (size_t i = 0U; i < configured_channel_count; ++i) {
        s_hop_channels[i] = (uint8_t)(
            (have_active_config ? active_config.first_channel : 1U) + i);
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
        .channel_count = configured_channel_count,
        /* Channel 0 belongs to OTA. Both peers use this shared seed to derive
         * the same deterministic audio hopping order. */
        .hop_seed = have_active_config
            ? active_config.seed : OTA_FHSS_DEFAULT_SEED,
        .generation = have_active_config ? active_config.generation : 0U,
        .reserved_channel = have_active_config
            ? active_config.reserved_channel : 0U,
        .slot_duration_us = have_active_config
            ? active_config.slot_duration_us : 300000U,
        .channel_switch_guard_us = have_active_config
            ? active_config.channel_switch_guard_us : 5000U,
        /* 재배정(2026-08-17): 판정 허용 오차를 channel_switch_guard_us(5ms)
         * 재사용에서 분리 — 실제 GDO0 ISR 지연/스케줄링 지터 흡수엔 5ms가
         * 타이트해서, 패킷은 정상 수신됐는데 타이밍만 창을 벗어나 MISS로
         * 판정되는 사례가 있었음(fhss_service.h 주석 참고). */
        .timing_window_margin_us = 20000U,
        .sync_offset_us = 0U,
        /* Adaptive first-order phase correction. Sub-500us variation is
         * treated as jitter; larger in-window drift is corrected gradually. */
        .correction_deadband_us = 500U,
        .correction_fast_threshold_us = 2000U,
        .correction_slow_divisor = 8U,
        .correction_fast_divisor = 2U,
        /* Do not mistake a one-slot FreeRTOS/SPI scheduling delay for clock
         * drift; cap one observation and converge over several valid SYNCs. */
        .correction_max_step_us = 500U,
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
        /* 음성용 기본값. OTA 진입/종료 시에만 아래 OTA_* 값으로 바꿨다가
         * 되돌린다 — 이유는 FHSS_SYNC_LOSS_COUNT_VOICE 주석 참고. */
        .loss_count = FHSS_SYNC_LOSS_COUNT_VOICE,
        .recovery_entry_miss_count = FHSS_RECOVERY_ENTRY_MISS_VOICE,
        /* 음성은 본문 읽기 타임아웃을 예전처럼 무선 오류로 취급한다
         * (=기존 동작 100% 유지). OTA에서만 완화 — 자세한 이유는
         * fhss_service.h의 treat_body_timeout_as_radio_error 주석 참고. */
        .treat_body_timeout_as_radio_error = true,
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
    ESP_LOGI(TAG, "ready: RX standby, GDO0=GPIO%d generation=%lu source=%s",
             CC1101_GDO0_GPIO,
             (unsigned long)service_config.generation,
             have_active_config ? "nvs-active" : "factory-default");
    return true;
}

bool fhss_audio_adapter_begin_tx(void)
{
    if (!s_adapter.initialized || s_adapter.tx_active || s_adapter.ota_active) {
        return false;
    }
    s_adapter.frame_count = 0U;
    s_adapter.tx_sequence = 0U;
    s_adapter.tx_packet_count = 0U;
    s_adapter.tx_session_id++;
    if (s_adapter.tx_session_id == 0U) {
        s_adapter.tx_session_id = 1U;
    }
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
    /* Flush real audio first, then send a control-only END packet. The old
     * audio flag could not represent a PTT release when no frame was pending. */
    bool ok = send_buffered_frames(0U);
    if (!send_end_packets()) {
        ok = false;
    }
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

bool fhss_audio_adapter_begin_ota(void)
{
    if (!s_adapter.initialized || s_adapter.tx_active || s_adapter.ota_active) {
        return false;
    }
    if (xSemaphoreTake(s_adapter.radio_mutex, portMAX_DELAY) != pdTRUE) {
        return false;
    }
    bool ok = fhss_service_pause(&s_adapter.service);
    if (ok) {
        ok = rf_transport_set_channel(
                 &s_adapter.service.radio, OTA_FIXED_CHANNEL) ==
             RF_TRANSPORT_STATUS_OK;
    }
    if (ok) {
        ok = rf_transport_start_receive(&s_adapter.service.radio) ==
             RF_TRANSPORT_STATUS_OK;
    }
    s_adapter.ota_active = ok;
    s_adapter.ota_fhss_active = false;
    if (s_adapter.ota_rx_queue != NULL) {
        xQueueReset(s_adapter.ota_rx_queue);
    }
    xSemaphoreGive(s_adapter.radio_mutex);
    if (!ok) {
        (void)fhss_service_set_role(
            &s_adapter.service, FHSS_SERVICE_ROLE_RX);
        ESP_LOGE(TAG, "failed to enter fixed-channel OTA mode");
        return false;
    }
    /* [2026-08-25] OTA 진행 중에만 rf_transport의 wait_until_ready()
     * busy-poll 루프에 yield를 켠다 — 음성은 이 호출을 안 타므로 항상
     * 꺼진 채(기존 동작 그대로) 남는다. 상세: rf_transport.h 주석,
     * gateway-ota design-notes 70절(gap-tuning 이후 interrupt wdt
     * timeout 크래시 원인 추정). */
    rf_transport_set_ota_mode(true);
    ESP_LOGI(TAG, "OTA mode started on CHANNR=%u", OTA_FIXED_CHANNEL);
    return true;
}

esp_err_t fhss_audio_adapter_activate_ota_fhss(
    const ota_fhss_config_fields_t *config)
{
    if (!s_adapter.initialized || !s_adapter.ota_active || config == NULL ||
        !ota_fhss_config_is_valid(config) || config->first_channel != 1U ||
        config->rendezvous_channel != 1U || config->reserved_channel != 0U ||
        config->channel_count > FHSS_AUDIO_HOP_CHANNEL_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(s_adapter.radio_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    for (size_t i = 0U; i < config->channel_count; ++i) {
        s_hop_channels[i] = (uint8_t)(config->first_channel + i);
    }
    s_adapter.service.config.channels = s_hop_channels;
    s_adapter.service.config.channel_count = config->channel_count;
    s_adapter.service.config.hop_seed = config->seed;
    s_adapter.service.config.generation = config->generation;
    s_adapter.service.config.reserved_channel = config->reserved_channel;
    s_adapter.service.config.slot_duration_us = config->slot_duration_us;
    s_adapter.service.config.channel_switch_guard_us =
        config->channel_switch_guard_us;
    /* [2026-08-25] OTA는 ACK를 계속 송신하느라 자기 SYNC를 자주 놓치므로
     * 하드 로스 판정을 완화한다. 아래 set_role()이 reset_controller()를
     * 거치면서 이 값을 다시 읽어간다. */
    s_adapter.service.config.loss_count = FHSS_SYNC_LOSS_COUNT_OTA;
    s_adapter.service.config.recovery_entry_miss_count =
        FHSS_RECOVERY_ENTRY_MISS_OTA;
    /* OTA에서만 본문 읽기 타임아웃 완화를 켠다 — 이게 켜져야 ACK가 다음
     * 홉 채널로 밀려 유실되는 문제가 풀린다(design-notes 50절). */
    s_adapter.service.config.treat_body_timeout_as_radio_error = false;
    if (s_adapter.ota_rx_queue != NULL) {
        xQueueReset(s_adapter.ota_rx_queue);
    }
    s_adapter.ota_fhss_active = true;
    const bool ok = fhss_service_set_role(
        &s_adapter.service, FHSS_SERVICE_ROLE_RX);
    if (!ok) {
        s_adapter.ota_fhss_active = false;
        (void)rf_transport_set_channel(
            &s_adapter.service.radio, OTA_FIXED_CHANNEL);
        (void)rf_transport_start_receive(&s_adapter.service.radio);
    }
    xSemaphoreGive(s_adapter.radio_mutex);
    return ok ? ESP_OK : ESP_FAIL;
}

bool fhss_audio_adapter_get_ota_fhss_generation(uint32_t *generation)
{
    if (!s_adapter.ota_fhss_active || generation == NULL) {
        return false;
    }
    *generation = s_adapter.service.config.generation;
    return true;
}

bool fhss_audio_adapter_end_ota(void)
{
    if (!s_adapter.initialized || !s_adapter.ota_active) {
        return true;
    }
    /* begin_ota()에서 켠 걸 되돌림 — 음성으로 완전히 돌아가는 시점에
     * 바로 꺼서, OTA 관련 상태가 하나도 안 남게 한다. */
    rf_transport_set_ota_mode(false);
    if (xSemaphoreTake(s_adapter.radio_mutex, portMAX_DELAY) != pdTRUE) {
        return false;
    }
    s_adapter.ota_active = false;
    s_adapter.ota_fhss_active = false;
    /* [2026-08-25] OTA에서만 완화했던 하드 로스 임계값을 음성용으로
     * 되돌린다 — 음성은 상대가 범위를 벗어났을 때 빨리 판정하고 고정
     * 채널로 돌아와야 다음 송신을 놓치지 않는다. 아래 set_role()이
     * reset_controller()를 거치면서 이 값을 다시 읽어간다. */
    s_adapter.service.config.loss_count = FHSS_SYNC_LOSS_COUNT_VOICE;
    s_adapter.service.config.recovery_entry_miss_count =
        FHSS_RECOVERY_ENTRY_MISS_VOICE;
    s_adapter.service.config.treat_body_timeout_as_radio_error = true;
    const bool ok = fhss_service_set_role(
        &s_adapter.service, FHSS_SERVICE_ROLE_RX);
    xSemaphoreGive(s_adapter.radio_mutex);
    if (ok) {
        ESP_LOGI(TAG, "OTA mode ended; FHSS RX resumed");
    }
    return ok;
}

fhss_audio_adapter_ota_rx_status_t fhss_audio_adapter_ota_receive(
    uint8_t *packet,
    size_t capacity,
    size_t *out_length,
    uint32_t timeout_ms
)
{
    if (!s_adapter.ota_active || packet == NULL || out_length == NULL ||
        capacity == 0U || timeout_ms == 0U) {
        return FHSS_AUDIO_ADAPTER_OTA_RX_ERROR;
    }
    if (s_adapter.ota_fhss_active) {
        fhss_ota_rx_item_t item = {0};
        if (xQueueReceive(
                s_adapter.ota_rx_queue, &item,
                pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
            return FHSS_AUDIO_ADAPTER_OTA_RX_TIMEOUT;
        }
        if (item.length > capacity) {
            return FHSS_AUDIO_ADAPTER_OTA_RX_ERROR;
        }
        memcpy(packet, item.data, item.length);
        *out_length = item.length;
        return FHSS_AUDIO_ADAPTER_OTA_RX_OK;
    }
    if (xSemaphoreTake(s_adapter.radio_mutex, portMAX_DELAY) != pdTRUE) {
        return FHSS_AUDIO_ADAPTER_OTA_RX_ERROR;
    }
    rf_transport_rx_packet_t received = {0};
    const rf_transport_status_t status = rf_transport_receive_packet(
        &s_adapter.service.radio, timeout_ms, &received);
    xSemaphoreGive(s_adapter.radio_mutex);
    if (status == RF_TRANSPORT_STATUS_TIMEOUT) {
        return FHSS_AUDIO_ADAPTER_OTA_RX_TIMEOUT;
    }
    if (status != RF_TRANSPORT_STATUS_OK || received.length == 0U ||
        received.length > capacity) {
        fhss_ota_diag_log_rx_result(
            "FIXED", OTA_FIXED_CHANNEL, (int)status, received.crc_ok,
            received.rssi_dbm, received.lqi, received.length);
        return FHSS_AUDIO_ADAPTER_OTA_RX_ERROR;
    }
    fhss_ota_diag_log_rx_result(
        "FIXED", OTA_FIXED_CHANNEL, (int)status, received.crc_ok,
        received.rssi_dbm, received.lqi, received.length);
    fhss_ota_diag_log_packet(
        "RX", "FIXED", OTA_FIXED_CHANNEL,
        received.payload, received.length);
    if (!received.crc_ok) {
        return FHSS_AUDIO_ADAPTER_OTA_RX_CRC_ERROR;
    }
    memcpy(packet, received.payload, received.length);
    *out_length = received.length;
    return FHSS_AUDIO_ADAPTER_OTA_RX_OK;
}

bool fhss_audio_adapter_ota_send(const uint8_t *packet, size_t length)
{
    if (!s_adapter.ota_active || packet == NULL || length == 0U ||
        length > RF_TRANSPORT_MAX_PACKET_LENGTH) {
        return false;
    }
    if (s_adapter.ota_fhss_active) {
        fhss_ota_diag_log_packet(
            "TX_QUEUE", "FHSS", s_adapter.service.current_channel,
            packet, length);
        const bool queued = fhss_service_send_data(
            &s_adapter.service, packet, length);
        fhss_ota_diag_log_tx_result(
            "FHSS_QUEUE", s_adapter.service.current_channel,
            queued ? 0 : -1, length);
        return queued;
    }
    if (xSemaphoreTake(s_adapter.radio_mutex, portMAX_DELAY) != pdTRUE) {
        return false;
    }
    const rf_transport_status_t send_status = rf_transport_send_packet(
        &s_adapter.service.radio, packet, (uint8_t)length);
    const rf_transport_status_t rx_status = rf_transport_start_receive(
        &s_adapter.service.radio);
    xSemaphoreGive(s_adapter.radio_mutex);
    fhss_ota_diag_log_packet(
        "TX", "FIXED", OTA_FIXED_CHANNEL, packet, length);
    fhss_ota_diag_log_tx_result(
        "FIXED", OTA_FIXED_CHANNEL, (int)send_status, length);
    return send_status == RF_TRANSPORT_STATUS_OK &&
           rx_status == RF_TRANSPORT_STATUS_OK;
}
