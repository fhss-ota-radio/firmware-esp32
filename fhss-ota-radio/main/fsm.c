#include "fsm.h"

#include <stdbool.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"

#include "audio_codec.h"
#include "audio_io.h"
#include "device_id.h"
#include "display_ui.h"
#include "fhss_audio_adapter.h"
#include "fhss_config_store.h"
#include "fhss_audio_pcm_test.h"
#include "firmware_version.h"
#include "ota_client.h"
#include "ota_protocol.h"
#include "ptt_button.h"
#include "rotary_encoder.h"
#include "status_led.h"

static const char *TAG = "fsm";

#define OTA_DISCOVER_BACKOFF_MAX_MS 100U

static uint32_t ota_discovery_random_callback(void *context);
static esp_err_t ota_fhss_activate_callback(
    const ota_fhss_config_fields_t *config, void *context);

/* 마이크가 없어도 codec -> FHSS -> CC1101 -> codec -> speaker 전체 경로를
 * 실기기에서 확인하기 위한 임시 모드다. 0이면 기존 마이크 캡처를 사용한다. */
#define FHSS_AUDIO_PCM_TEST_ENABLED 0

static QueueHandle_t s_event_queue;
static fsm_state_t s_state = FSM_STATE_BOOT_INIT;

/*
 * 오디오 프레임 전용 큐. fsm_event_t(이벤트 큐)는 페이로드가 없는 enum이라
 * 데이터를 못 실으므로, 수신 프레임 바이트는 이 큐로 따로 옮기고 이벤트
 * 큐에는 "도착했다"는 신호(FSM_EVENT_RX_FRAME)만 올린다.
 *
 * 재배정(2026-08-17): 깊이 4(80ms 버퍼)였을 때 실기기 FHSS RX 테스트에서
 * "rx audio queue full, dropping frame"이 반복되며 재생이 끊기는(두두두
 * 소리) 문제가 있었음 — MENU_COMM -> RX_AUDIO 전이 직후 rx_audio_task가
 * 실제로 큐를 비우기 시작하기 전 짧은 구간에 프레임이 몰려서 초반에 버스트로
 * 드롭되는 것으로 보임. 아이템 하나가 몇십 바이트 수준이라 메모리 부담은
 * 무시할 만해서, 그 초기 버스트를 넉넉히 흡수하도록 32개(20ms/프레임 기준
 * 약 640ms 버퍼)로 올림.
 */
#define FSM_RX_AUDIO_FRAME_MAX_BYTES AUDIO_CODEC_MAX_ENCODED_BYTES
#define FSM_RX_AUDIO_QUEUE_DEPTH     32

typedef struct {
    uint8_t data[FSM_RX_AUDIO_FRAME_MAX_BYTES];
    size_t len;
} fsm_rx_audio_frame_t;

static QueueHandle_t s_rx_audio_queue;
static TaskHandle_t s_ota_radio_task;
static volatile bool s_ota_radio_should_stop;
static SemaphoreHandle_t s_ota_response_done;
static volatile bool s_ota_ready_to_reboot;
static TaskHandle_t s_ota_reboot_task;
static TimerHandle_t s_ota_fhss_sync_timer;
static TimerHandle_t s_ota_fhss_resync_timer;

#define OTA_RADIO_RX_TIMEOUT_MS 40U
#define OTA_RESPONSE_WAIT_MS    250U
#define OTA_REBOOT_DELAY_MS     250U
#define OTA_FHSS_SYNC_TIMEOUT_MS 5000U
#define OTA_FHSS_RESYNC_GRACE_MS 3000U

static void ota_fhss_sync_timeout_callback(TimerHandle_t timer)
{
    (void)timer;
    fsm_post_event(FSM_EVENT_FHSS_SYNC_TIMEOUT);
}

static void ota_fhss_resync_timeout_callback(TimerHandle_t timer)
{
    (void)timer;
    fsm_post_event(FSM_EVENT_OTA_FHSS_RESYNC_TIMEOUT);
}

_Static_assert(
    OTA_DISCOVER_BACKOFF_MAX_MS + OTA_RADIO_RX_TIMEOUT_MS <
        OTA_RESPONSE_WAIT_MS,
    "DISCOVER backoff must leave time for response TX and RX re-arm"
);

static void ota_reboot_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(OTA_REBOOT_DELAY_MS));
    ESP_LOGI(TAG, "final END ACK sent; restarting into OTA partition");
    esp_restart();
}

static esp_err_t ota_radio_send_callback(
    const uint8_t *packet,
    size_t packet_length,
    void *context
)
{
    (void)context;
    const esp_err_t result = fhss_audio_adapter_ota_send(packet, packet_length)
        ? ESP_OK
        : ESP_FAIL;

    ota_packet_type_t response_type;
    ota_ack_fields_t ack;
    const bool final_end_ack =
        result == ESP_OK &&
        s_ota_ready_to_reboot &&
        ota_protocol_decode_ack(
            packet, packet_length, &response_type, &ack) &&
        response_type == OTA_PKT_ACK &&
        ack.acknowledged_type == (uint8_t)OTA_PKT_END;

    if (s_ota_response_done != NULL) {
        xSemaphoreGive(s_ota_response_done);
    }

    /* ota_writer_finish() has already selected the new boot partition.  The
     * synchronous radio send above reaches IDLE only after the complete END
     * ACK has left the CC1101, so rebooting from a separate task cannot cut
     * the final handshake short. */
    if (final_end_ack && s_ota_reboot_task == NULL) {
        if (xTaskCreate(
                ota_reboot_task,
                "ota_reboot",
                2048U,
                NULL,
                tskIDLE_PRIORITY + 2U,
                &s_ota_reboot_task) != pdPASS) {
            s_ota_reboot_task = NULL;
            /* The ACK is already completely transmitted.  A short blocking
             * fallback is safer than leaving a verified image selected but
             * never rebooting solely because task allocation failed. */
            ESP_LOGW(TAG, "reboot task allocation failed; using inline delay");
            vTaskDelay(pdMS_TO_TICKS(OTA_REBOOT_DELAY_MS));
            esp_restart();
        }
    }
    return result;
}

static void ota_radio_task(void *arg)
{
    (void)arg;
    uint8_t packet[OTA_CLIENT_MAX_PACKET_LENGTH];
    uint32_t consecutive_timeouts = 0U;
    while (!s_ota_radio_should_stop) {
        size_t packet_length = 0U;
        const fhss_audio_adapter_ota_rx_status_t status =
            fhss_audio_adapter_ota_receive(
                packet,
                sizeof(packet),
                &packet_length,
                OTA_RADIO_RX_TIMEOUT_MS);
        if (status == FHSS_AUDIO_ADAPTER_OTA_RX_OK) {
            consecutive_timeouts = 0U;
            if (s_ota_response_done != NULL) {
                (void)xSemaphoreTake(s_ota_response_done, 0U);
            }
            const esp_err_t err = ota_client_submit_packet(
                packet, packet_length);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "OTA RX queue submit failed: %s",
                         esp_err_to_name(err));
            } else if (s_ota_response_done != NULL) {
                /* Do not re-enter RX before the lower-priority consumer has
                 * encoded and transmitted ACK/NACK on the same half-duplex
                 * radio. Malformed packets legitimately time out here. */
                (void)xSemaphoreTake(
                    s_ota_response_done,
                    pdMS_TO_TICKS(OTA_RESPONSE_WAIT_MS));
            }
        } else if (status == FHSS_AUDIO_ADAPTER_OTA_RX_CRC_ERROR) {
            consecutive_timeouts = 0U;
            ESP_LOGW(TAG, "OTA RF packet dropped: CC1101 CRC failed");
        } else if (status == FHSS_AUDIO_ADAPTER_OTA_RX_ERROR &&
                   !s_ota_radio_should_stop) {
            ESP_LOGE(TAG, "OTA RF receive failed");
            fsm_post_event(FSM_EVENT_ERROR);
            break;
        } else {
            consecutive_timeouts++;
            if ((consecutive_timeouts % 50U) == 0U) {
                ESP_LOGI(
                    "OTA_DIAG",
                    "LISTEN no-packet count=%" PRIu32
                    " window_ms=%" PRIu32 " fsm=%s",
                    consecutive_timeouts,
                    consecutive_timeouts * OTA_RADIO_RX_TIMEOUT_MS,
                    fsm_state_name(fsm_get_state()));
            }
            /* Allow timeout-driven NACK processing to acquire the radio mutex. */
            vTaskDelay(1U);
        }
    }
    s_ota_radio_task = NULL;
    vTaskDelete(NULL);
}

static bool start_ota_radio(void)
{
    if (s_ota_radio_task != NULL) {
        return true;
    }
    if (!fhss_audio_adapter_begin_ota()) {
        return false;
    }
    s_ota_radio_should_stop = false;
    if (xTaskCreate(
            ota_radio_task,
            "ota_radio",
            4096U,
            NULL,
            tskIDLE_PRIORITY + 4U,
            &s_ota_radio_task) != pdPASS) {
        s_ota_radio_task = NULL;
        (void)fhss_audio_adapter_end_ota();
        return false;
    }
    return true;
}

static bool stop_ota_radio(void)
{
    if (s_ota_radio_task != NULL) {
        s_ota_radio_should_stop = true;
        while (s_ota_radio_task != NULL) {
            vTaskDelay(pdMS_TO_TICKS(5U));
        }
        s_ota_radio_should_stop = false;
    }
    return fhss_audio_adapter_end_ota();
}

/* RF 태스크에서 Speex decode/I2S 재생까지 실행하면 다음 홉 수신이 늦어진다.
 * 따라서 수신 frame은 팀 FSM이 이미 소유한 오디오 큐로 넘겨 RX 태스크가 재생한다. */
static bool on_fhss_rx_audio_frame(const uint8_t *frame, size_t length, void *context)
{
    (void)context;
    return fsm_post_rx_audio_frame(frame, length);
}

/* 정상적인 슬롯 보정은 FHSS 내부에서 처리한다. TALKSPURT_ENDED만 RX_AUDIO를
 * 즉시 정상 종료하기 위해 RX_DONE으로 변환한다. 이 연결이 없으면 PTT를 놓은 뒤
 * 수신 측이 1초 무음 timeout까지 기다리고 이후 SYNC_LOST로 오인할 수 있다. */
static void on_fhss_audio_event(fhss_audio_adapter_event_t event, void *context)
{
    (void)context;
    switch (event) {
    case FHSS_AUDIO_ADAPTER_EVENT_SYNC_ACQUIRED: {
        uint32_t generation = 0U;
        if (fhss_audio_adapter_get_ota_fhss_generation(&generation)) {
            const esp_err_t err = fhss_config_store_activate(generation);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "failed to promote synchronized FHSS generation: %s",
                         esp_err_to_name(err));
                fsm_post_event(FSM_EVENT_ERROR);
                break;
            }
            fsm_post_event(FSM_EVENT_SYNC_ACQUIRED);
        }
        break;
    }
    case FHSS_AUDIO_ADAPTER_EVENT_SYNC_LOST:
        fsm_post_event(FSM_EVENT_SYNC_LOST);
        break;
    case FHSS_AUDIO_ADAPTER_EVENT_TALKSPURT_ENDED:
        fsm_post_event(FSM_EVENT_RX_DONE);
        break;
    case FHSS_AUDIO_ADAPTER_EVENT_ERROR:
    default:
        fsm_post_event(FSM_EVENT_ERROR);
        break;
    }
}

/* 상태별 이름/이벤트별 이름: 로그 및 OLED 표시용 */
static const char *s_state_names[FSM_STATE_COUNT] = {
    [FSM_STATE_BOOT_INIT]     = "BOOT_INIT",
    [FSM_STATE_MENU_COMM]     = "MENU_COMM",
    [FSM_STATE_MENU_IDLE]     = "MENU_IDLE",
    [FSM_STATE_MENU_OTA]      = "MENU_OTA",
    [FSM_STATE_OTA_FHSS_CONFIGURED] = "OTA_FHSS_CONFIGURED",
    [FSM_STATE_OTA_FHSS_SYNCING] = "OTA_FHSS_SYNCING",
    [FSM_STATE_OTA_FHSS_READY] = "OTA_FHSS_READY",
    [FSM_STATE_TX_AUDIO]      = "TX_AUDIO",
    [FSM_STATE_RX_AUDIO]      = "RX_AUDIO",
    [FSM_STATE_OTA_RECEIVING] = "OTA_RECEIVING",
    [FSM_STATE_OTA_APPLYING]  = "OTA_APPLYING",
    [FSM_STATE_ERROR]         = "ERROR",
};

static const char *s_event_names[FSM_EVENT_COUNT] = {
    [FSM_EVENT_INIT_DONE]      = "INIT_DONE",
    [FSM_EVENT_SYNC_LOST]      = "SYNC_LOST",
    [FSM_EVENT_MENU_SELECT_COMM] = "MENU_SELECT_COMM",
    [FSM_EVENT_MENU_SELECT_IDLE] = "MENU_SELECT_IDLE",
    [FSM_EVENT_MENU_SELECT_OTA]  = "MENU_SELECT_OTA",
    [FSM_EVENT_FHSS_CONFIG_READY] = "FHSS_CONFIG_READY",
    [FSM_EVENT_FHSS_ACTIVATE] = "FHSS_ACTIVATE",
    [FSM_EVENT_SYNC_ACQUIRED] = "SYNC_ACQUIRED",
    [FSM_EVENT_FHSS_SYNC_TIMEOUT] = "FHSS_SYNC_TIMEOUT",
    [FSM_EVENT_OTA_FHSS_RESYNC_TIMEOUT] = "OTA_FHSS_RESYNC_TIMEOUT",
    [FSM_EVENT_PTT_PRESS]      = "PTT_PRESS",
    [FSM_EVENT_PTT_RELEASE]    = "PTT_RELEASE",
    [FSM_EVENT_RX_FRAME]       = "RX_FRAME",
    [FSM_EVENT_RX_DONE]        = "RX_DONE",
    [FSM_EVENT_OTA_START]      = "OTA_START",
    [FSM_EVENT_OTA_CHUNK]      = "OTA_CHUNK",
    [FSM_EVENT_OTA_COMPLETE]   = "OTA_COMPLETE",
    [FSM_EVENT_OTA_VERIFY_OK]  = "OTA_VERIFY_OK",
    [FSM_EVENT_OTA_VERIFY_FAIL] = "OTA_VERIFY_FAIL",
    [FSM_EVENT_ERROR]          = "ERROR",
    [FSM_EVENT_RETRY]          = "RETRY",
};

/*
 * 상태별 전이. 전역 전이(EV_ERROR, EV_SYNC_LOST)는 fsm_task()에서 별도 처리하므로 여기 넣지 않는다.
 *
 * 브로드캐스트 방식이라 "동기 획득 대기" 상태가 없다 — PTT 누른 쪽이 정해진
 * 시작 채널로 먼저 송신하고 그 순간부터 시드 기반으로 제멋대로 호핑하며,
 * 받는 쪽은 그 수신 시점을 기준으로 같은 시드로 호핑을 추종한다. 그래서
 * BOOT_INIT 이후엔 곧바로 MENU_COMM(정해진 채널에서 통신 대기)으로 들어간다.
 *
 * EV_SYNC_LOST는 그래도 전역 안전장치로 남겨둔다 — 무선 계층(rf_transport/
 * fhss_core)이 홉 추종 중 연속 미수신 등으로 타이밍이 완전히 깨졌다고
 * 판단하면 이 이벤트를 올리고, FSM은 지금 어느 상태에 있든 MENU_COMM(정해진
 * 채널)으로 강제 복귀한다. 정상적인 한 세션 종료(TX_AUDIO의 PTT_RELEASE,
 * RX_AUDIO의 RX_DONE)와는 별개의, 진짜 이상 상황용 탈출구다.
 *
 * 음성(FHSS)과 OTA는 같은 CC1101 라디오를 쓰는 단일 반이중 트랜시버이므로,
 * OTA_RECEIVING 진입 시 음성 호핑 이탈은 정책이 아니라 하드웨어 제약이다.
 *
 * MENU_COMM/MENU_IDLE/MENU_OTA는 수신 패킷 해석을 게이팅하는 3-way 메뉴
 * 모드다(2026-08-11 UI 재설계로 MENU_IDLE이 "뮤트"로 새로 추가됨 — 기존
 * MENU_IDLE은 MENU_COMM으로 개명). MENU_COMM=음성 통신, MENU_OTA=펌웨어
 * 청크, MENU_IDLE=뮤트(수신 처리 없음, PTT/RX_FRAME 전이도 없음). 메뉴 전환
 * 이벤트(MENU_SELECT_COMM/IDLE/OTA)는 이 세 상태끼리만 정의돼있고 TX_AUDIO/
 * RX_AUDIO/OTA_RECEIVING/OTA_APPLYING에는 없다 — 그 상태에서 메뉴 전환
 * 이벤트가 오면 unhandled로 무시되며, 이게 곧 "활동 중 메뉴 변경 불가"의
 * 구현이다. 같은 이유로 OTA_START는 MENU_OTA에서만, PTT_PRESS/RX_FRAME은
 * MENU_COMM에서만 유효해서 서로 끼어들 수 없다.
 */
typedef struct {
    fsm_state_t state;
    fsm_event_t event;
    fsm_state_t next_state;
} fsm_transition_t;

static const fsm_transition_t s_transitions[] = {
    { FSM_STATE_BOOT_INIT,     FSM_EVENT_INIT_DONE,      FSM_STATE_MENU_COMM },

    { FSM_STATE_MENU_COMM,     FSM_EVENT_PTT_PRESS,      FSM_STATE_TX_AUDIO },
    { FSM_STATE_MENU_COMM,     FSM_EVENT_RX_FRAME,       FSM_STATE_RX_AUDIO },
    { FSM_STATE_MENU_COMM,     FSM_EVENT_MENU_SELECT_IDLE, FSM_STATE_MENU_IDLE },
    { FSM_STATE_MENU_COMM,     FSM_EVENT_MENU_SELECT_OTA, FSM_STATE_MENU_OTA },

    { FSM_STATE_MENU_IDLE,     FSM_EVENT_MENU_SELECT_COMM, FSM_STATE_MENU_COMM },
    { FSM_STATE_MENU_IDLE,     FSM_EVENT_MENU_SELECT_OTA, FSM_STATE_MENU_OTA },

    { FSM_STATE_MENU_OTA,      FSM_EVENT_MENU_SELECT_COMM, FSM_STATE_MENU_COMM },
    { FSM_STATE_MENU_OTA,      FSM_EVENT_MENU_SELECT_IDLE, FSM_STATE_MENU_IDLE },
    { FSM_STATE_MENU_OTA,      FSM_EVENT_FHSS_CONFIG_READY, FSM_STATE_OTA_FHSS_CONFIGURED },

    { FSM_STATE_OTA_FHSS_CONFIGURED, FSM_EVENT_FHSS_CONFIG_READY, FSM_STATE_OTA_FHSS_CONFIGURED },
    { FSM_STATE_OTA_FHSS_CONFIGURED, FSM_EVENT_FHSS_ACTIVATE, FSM_STATE_OTA_FHSS_SYNCING },
    { FSM_STATE_OTA_FHSS_CONFIGURED, FSM_EVENT_MENU_SELECT_COMM, FSM_STATE_MENU_COMM },
    { FSM_STATE_OTA_FHSS_CONFIGURED, FSM_EVENT_MENU_SELECT_IDLE, FSM_STATE_MENU_IDLE },

    { FSM_STATE_OTA_FHSS_SYNCING, FSM_EVENT_SYNC_ACQUIRED, FSM_STATE_OTA_FHSS_READY },
    { FSM_STATE_OTA_FHSS_SYNCING, FSM_EVENT_FHSS_SYNC_TIMEOUT, FSM_STATE_MENU_OTA },

    { FSM_STATE_OTA_FHSS_READY, FSM_EVENT_OTA_START, FSM_STATE_OTA_RECEIVING },
    { FSM_STATE_OTA_FHSS_READY, FSM_EVENT_SYNC_LOST, FSM_STATE_MENU_OTA },
    { FSM_STATE_OTA_FHSS_READY, FSM_EVENT_MENU_SELECT_COMM, FSM_STATE_MENU_COMM },
    { FSM_STATE_OTA_FHSS_READY, FSM_EVENT_MENU_SELECT_IDLE, FSM_STATE_MENU_IDLE },

    { FSM_STATE_TX_AUDIO,      FSM_EVENT_PTT_RELEASE,    FSM_STATE_MENU_COMM },

    /* RF packet 하나의 두 Speex frame이 연속 도착해도 두 번째 RX_FRAME이
     * 현재 RX 태스크를 재시작하지 않고 정상적으로 큐에 누적되게 한다. */
    { FSM_STATE_RX_AUDIO,      FSM_EVENT_RX_FRAME,       FSM_STATE_RX_AUDIO },
    { FSM_STATE_RX_AUDIO,      FSM_EVENT_RX_DONE,        FSM_STATE_MENU_COMM },

    { FSM_STATE_OTA_RECEIVING, FSM_EVENT_OTA_CHUNK,      FSM_STATE_OTA_RECEIVING },
    { FSM_STATE_OTA_RECEIVING, FSM_EVENT_OTA_COMPLETE,   FSM_STATE_OTA_APPLYING },

    { FSM_STATE_OTA_APPLYING,  FSM_EVENT_OTA_VERIFY_FAIL, FSM_STATE_MENU_OTA },

    { FSM_STATE_ERROR,         FSM_EVENT_RETRY,          FSM_STATE_BOOT_INIT },
};

/*
 * UI 컴포넌트(display_ui/ptt_button/rotary_encoder) 콜백 — 각 컴포넌트는 FSM을
 * 모르므로, 이 콜백들이 fsm_post_event()로 매핑하는 접착부 역할을 한다.
 * rf_transport 등 아직 없는 컴포넌트가 직접 호출해야 할 지점(fsm_post_rx_audio_frame
 * 등)은 그대로 TODO로 남겨둔다 — 지금 여기서 채우면 실제 API가 없는 상태라
 * 추측성 코드가 되고, 나중에 다시 갈아엎어야 한다.
 */
/*
 * TEMP(INMP441 실배선 테스트용, LOOPBACK_ENABLE로 켜고 끔 —
 * audio_io_config.h의 매크로 정의 참고): MENU_IDLE(뮤트)에서 PTT를 누르면
 * 정식 전이표를 타지 않고(MENU_IDLE은 설계상 PTT 전이가 없음) 이 loopback
 * 테스트로만 빠진다 — 삐빅음 없이 마이크로 캡처한 프레임을 Speex로
 * 인코딩/디코딩 왕복시켜 버퍼에 모아뒀다가 PTT를 떼면 1초 뒤 재생한다.
 * INMP441 실측 캡처 신호가 원래 작아서(peak 846/32767 수준, 2026-08-11
 * 실기기 확인) 그냥 재생하면 안 들려서 소프트웨어 gain으로 증폭 후
 * 재생한다(MIC_TEST_PLAYBACK_GAIN). 정식 FSM 상태/전이는 전혀 건드리지
 * 않아 LOOPBACK_ENABLE을 끄면(기본값) 원래 동작으로 돌아간다.
 */
#ifdef LOOPBACK_ENABLE
#define MIC_TEST_MAX_FRAMES 250 /* 250 * 20ms = 5초 상한 */
#define MIC_TEST_PLAYBACK_GAIN 16.0f /* peak 846 기준 약 13536(풀스케일 41%)까지 증폭 */

typedef struct {
    uint8_t data[AUDIO_CODEC_MAX_ENCODED_BYTES];
    size_t len;
} mic_test_frame_t;

static TaskHandle_t s_mic_test_task;
static volatile bool s_mic_test_recording;

static void mic_test_task(void *arg)
{
    mic_test_frame_t *frames = malloc(sizeof(mic_test_frame_t) * MIC_TEST_MAX_FRAMES);
    size_t frame_count = 0;

    while (s_mic_test_recording && frame_count < MIC_TEST_MAX_FRAMES) {
        uint8_t enc[AUDIO_CODEC_MAX_ENCODED_BYTES];
        int n = audio_io_capture_encode(enc, sizeof(enc));
        if (n > 0) {
            memcpy(frames[frame_count].data, enc, (size_t)n);
            frames[frame_count].len = (size_t)n;
            frame_count++;
        }
    }

    /* TEMP(진단용): 캡처된 신호가 실제로 있는지(0에 가까우면 배선/슬롯 문제)
     * 재생 전에 피크로 먼저 확인 */
    int16_t peak = 0;
    for (size_t i = 0; i < frame_count; i++) {
        int16_t p = audio_io_decode_peek_peak(frames[i].data, frames[i].len);
        if (p > peak) {
            peak = p;
        }
    }
    ESP_LOGI(TAG, "mic test: %u frames captured, peak=%d/32767, playback in 1s",
             (unsigned)frame_count, peak);
    vTaskDelay(pdMS_TO_TICKS(1000));

    audio_io_speaker_enable();
    for (size_t i = 0; i < frame_count; i++) {
        audio_io_decode_play_scaled(frames[i].data, frames[i].len, MIC_TEST_PLAYBACK_GAIN);
    }
    audio_io_speaker_disable();

    free(frames);
    s_mic_test_task = NULL;
    vTaskDelete(NULL);
}
#endif /* LOOPBACK_ENABLE */

static void on_ptt_event(bool pressed, void *ctx)
{
    fsm_state_t state = fsm_get_state();

#ifdef LOOPBACK_ENABLE
    /* TEMP: MENU_IDLE에서는 정식 FSM 이벤트 대신 마이크 loopback 테스트로 라우팅.
     * 이것도 실제로 마이크를 캡처하는 "수음"이라 흰색 LED를 그대로 켠다. */
    if (state == FSM_STATE_MENU_IDLE) {
        if (pressed) {
            status_led_set_white_dim();
            if (s_mic_test_task == NULL) {
                s_mic_test_recording = true;
                xTaskCreate(mic_test_task, "mic_test", 8192, NULL, tskIDLE_PRIORITY + 3, &s_mic_test_task);
            }
        } else {
            status_led_off();
            s_mic_test_recording = false;
        }
        return;
    }
#endif /* LOOPBACK_ENABLE */

    /* status_led 흰색은 실제로 수음(TX_AUDIO 캡처)으로 이어지는 PTT일 때만
     * 켠다 — 전이표상 MENU_COMM에서 누른 PTT만 TX_AUDIO로 이어지고, 다른
     * 상태(예: MENU_OTA)에서 누르면 FSM이 그냥 무시하니 LED도 안 켜야
     * 맞다(이전엔 PTT 원시 입력을 상태와 무관하게 그대로 반영해서, 아무
     * 효과도 없는 상태에서 눌러도 흰색이 켜지는 문제가 있었음). 끌 때는
     * 반대로 상태와 무관하게 항상 꺼서, EV_ERROR/EV_SYNC_LOST 같은 전역
     * 전이로 도중에 TX_AUDIO를 벗어나도 흰색이 켜진 채로 안 남게 한다. */
    if (pressed) {
        if (state == FSM_STATE_MENU_COMM) {
            status_led_set_white_dim();
        }
    } else {
        status_led_off();
    }

    fsm_post_event(pressed ? FSM_EVENT_PTT_PRESS : FSM_EVENT_PTT_RELEASE);
}

/* rotary_encoder_menu_t <-> display_ui_menu_item_t는 순서가 같은 3-way라
 * 매핑이 자명하지만, 열거형이 나중에 각자 따로 바뀔 수 있으니 캐스트 대신
 * 명시적으로 변환한다. */
static display_ui_menu_item_t menu_item_from_rotary(rotary_encoder_menu_t cursor)
{
    switch (cursor) {
        case ROTARY_ENCODER_MENU_COMM: return DISPLAY_UI_MENU_COMM;
        case ROTARY_ENCODER_MENU_OTA:  return DISPLAY_UI_MENU_OTA;
        default:                       return DISPLAY_UI_MENU_IDLE;
    }
}

/* 현재 FSM 상태를 화면에 표시할 메뉴 항목으로 변환한다. TX_AUDIO/RX_AUDIO는
 * MENU_COMM에서만, OTA_RECEIVING/OTA_APPLYING은 MENU_OTA에서만 진입 가능하니
 * (전이표 참고) 그 상태들도 각각 COMM/OTA로 매핑하는 게 실제로 맞다. */
static display_ui_menu_item_t menu_item_from_fsm_state(fsm_state_t state)
{
    switch (state) {
        case FSM_STATE_MENU_COMM:
        case FSM_STATE_TX_AUDIO:
        case FSM_STATE_RX_AUDIO:
            return DISPLAY_UI_MENU_COMM;
        case FSM_STATE_MENU_IDLE:
            return DISPLAY_UI_MENU_IDLE;
        case FSM_STATE_MENU_OTA:
        case FSM_STATE_OTA_FHSS_CONFIGURED:
        case FSM_STATE_OTA_FHSS_SYNCING:
        case FSM_STATE_OTA_FHSS_READY:
        case FSM_STATE_OTA_RECEIVING:
        case FSM_STATE_OTA_APPLYING:
            return DISPLAY_UI_MENU_OTA;
        default:
            return DISPLAY_UI_MENU_COMM;
    }
}

static void on_menu_select(rotary_encoder_menu_t selected, void *ctx)
{
    (void)ctx;
    /* ERROR 화면에서는 메뉴 선택보다 복구가 우선이다. 기존에는 RETRY를
     * 발생시키는 입력 경로가 없어 사용자가 ERROR 화면에 영구 고정됐다. */
    if (fsm_get_state() == FSM_STATE_ERROR) {
        fsm_post_event(FSM_EVENT_RETRY);
        return;
    }

    fsm_event_t event;
    switch (selected) {
        case ROTARY_ENCODER_MENU_COMM: event = FSM_EVENT_MENU_SELECT_COMM; break;
        case ROTARY_ENCODER_MENU_OTA:  event = FSM_EVENT_MENU_SELECT_OTA;  break;
        default:                       event = FSM_EVENT_MENU_SELECT_IDLE; break;
    }
    fsm_post_event(event);
}

/* 로터리 회전만으로는 FSM 이벤트가 없다 (docs/fsm-design.md §메뉴 게이팅) —
 * 클릭으로 확정하기 전까지는 화면에 미리보기(흰 테두리)만 갱신한다. */
static void on_menu_cursor(rotary_encoder_menu_t cursor, void *ctx)
{
    display_ui_draw_menu(menu_item_from_fsm_state(fsm_get_state()), menu_item_from_rotary(cursor));
}

/*
 * TX_AUDIO 동안만 도는 캡처 태스크. PTT를 누르고 있는 동안(TX_AUDIO 상태인
 * 동안) 20ms마다 마이크를 읽어 Speex로 인코딩한다. rf_transport가 아직 없어
 * 인코딩된 프레임을 실제로 보낼 곳이 없으므로 그 부분만 TODO — 캡처/인코딩
 * 자체는 audio_io가 이미 있으니 추측 없이 그대로 동작한다.
 * MENU_COMM 진입(PTT_RELEASE) 시 on_enter_menu_comm()에서 태스크를 정리한다.
 *
 * "말하기 시작" 삐빅음은 여기(태스크 안)가 아니라 on_enter_tx_audio()에서,
 * 즉 이 태스크를 만들기 *전에* fsm_task 안에서 재생한다 — PTT를 아주 빠르게
 * 누르고 뗐을 때(삐빅음 재생 도중 PTT_RELEASE가 들어오는 경우)
 * vTaskDelete()가 이 태스크를 스피커 disable 전에 강제 종료시켜서 SD 핀이
 * HIGH로 계속 남아 삐빅음이 안 끝나고 계속 재생되는 버그가 있었다
 * (troubleshoot/요약.md, 2026-08-10). fsm_task는 이벤트를 한 번에 하나씩
 * 순서대로 처리하므로, 삐빅음 재생을 fsm_task 안에서 끝까지 블로킹으로 마친
 * 뒤에야 다음 이벤트(PTT_RELEASE)를 처리하게 만들어서 이 경쟁 상태를 없앴다.
 */
static TaskHandle_t s_tx_audio_task;
/* TEMP였던 vTaskDelete(s_tx_audio_task) 강제 종료를 없애기 위한 협조적
 * 종료 플래그 — stop_tx_audio_task() 참고. */
static volatile bool s_tx_audio_should_stop;

static void tx_audio_task(void *arg)
{
#if FHSS_AUDIO_PCM_TEST_ENABLED
    /* 160-sample PCM을 20 ms cadence로 생성해 실제 codec/RF 경로에 공급한다.
     * 태스크 수명은 마이크 경로와 동일하게 PTT PRESS~RELEASE로 제한된다. */
    fhss_audio_pcm_test_run();
#else
    uint8_t frame[AUDIO_CODEC_MAX_ENCODED_BYTES];

    while (!s_tx_audio_should_stop) {
        int n = audio_io_capture_encode(frame, sizeof(frame));
        if (n > 0) {
            /* 20 ms Speex frame 두 개를 adapter가 RF packet 하나로 묶는다. */
            if (!fhss_audio_adapter_submit_encoded_frame(frame, (size_t)n)) {
                ESP_LOGW(TAG, "encoded audio frame dropped: length=%d", n);
            }
        }
    }
#endif

    s_tx_audio_task = NULL;
    vTaskDelete(NULL);
}

/*
 * PTT_RELEASE/EV_ERROR/EV_SYNC_LOST로 TX_AUDIO를 벗어날 때 이 함수로
 * s_tx_audio_task를 정리한다. 예전엔 vTaskDelete(s_tx_audio_task)로 그
 * 자리에서 바로 죽였는데, `#else`(실제 마이크) 경로는 거의 항상
 * audio_io_capture_encode() -> i2s_channel_read(mic 채널) 안에 블로킹돼
 * 있어서, 하필 그 순간 강제 종료되면 I2S 드라이버가 그 채널을 "읽는 중"
 * 상태로 표시해둔 채 못 풀려서 이후 그 채널로의 모든 read가
 * ESP_ERR_INVALID_STATE(259)로 실패하는 버그가 있었다(마이크 loopback
 * 테스트에서 "mic read failed (err=259)"로 재현됨 — 스피커/삐빅음 쪽의
 * 동일 계열 버그를 고쳤던 troubleshoot/on_enter_tx_audio-beep_fix.md와
 * 같은 원인, 이쪽(캡처 루프 본체)은 그때 안 고쳐져 있었음).
 * `rx_audio_task`가 이미 쓰고 있는 "태스크가 스스로 끝낸다" 패턴을 여기도
 * 적용 — 큐 대기(rx_audio_task)와 달리 I2S read는 강제 종료해도 안전한
 * 자원이 아니라서, 플래그만 세우고 태스크가 다음 루프에서 스스로
 * vTaskDelete(NULL)하도록 기다린다. 정상 상황(마이크에서 데이터가 계속
 * 들어옴)이면 다음 20ms 프레임 주기 안에 곧바로 끝나고, 데이터가 전혀 안
 * 들어와도 i2s_channel_read()의 자체 타임아웃(AUDIO_IO_I2S_TIMEOUT_MS,
 * 1초)이 상한이라 무한 대기는 아니다.
 */
static void stop_tx_audio_task(void)
{
    if (s_tx_audio_task == NULL) {
        return;
    }
    s_tx_audio_should_stop = true;
    while (s_tx_audio_task != NULL) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    s_tx_audio_should_stop = false;
}

/*
 * RX_AUDIO 동안만 도는 재생 태스크. s_rx_audio_queue에서 프레임을 꺼내
 * audio_io_decode_play()로 재생한다. 오디오 프레임이 잠시 끊겨도 상대 PTT가
 * 해제됐다고 단정하지 않고 무음을 출력하며 RX_AUDIO를 유지한다.
 *
 * 정상 종료는 송신 측의 TALKSPURT_END 패킷이 올리는 FSM_EVENT_RX_DONE으로,
 * 비정상적인 장시간 무선 단절은 FHSS의 FSM_EVENT_SYNC_LOST로 처리한다. DATA
 * 지연을 세션 종료로 오판해 PTT 유지 중 MENU_COMM으로 떨어지는 일을 막는다.
 */
static TaskHandle_t s_rx_audio_task;
/* I2S write 또는 queue wait 도중 RX 태스크를 외부에서 강제 삭제하지 않는다.
 * 정상 timeout이 아닌 상태 전이에서 stop_rx_audio_task()가 이 플래그를 세우면
 * RX 태스크가 현재 작업을 마친 뒤 speaker channel까지 직접 정리한다. */
static volatile bool s_rx_audio_should_stop;

/* 새 프레임이 없는 idle 구간에서 xQueueReceive를 짧게 끊어 폴링하며
 * audio_io_write_silence()로 무음을 채워 넣는 주기(ms). 재배정(2026-08-17):
 * 예전엔 긴 시간 블로킹 대기해서 그동안 I2S write가 전혀 없었는데, I2S TX가
 * circular DMA라 write가 멈추면
 * 마지막 실제 음성 프레임 파형을 그대로 반복 재생 -> "두두두두" 잡음으로
 * 들리는 문제가 실기기에서 확인됨. 프레임 주기(20ms)와 맞춰 폴링. */
#define FSM_RX_AUDIO_POLL_MS 20U

static void rx_audio_task(void *arg)
{
    fsm_rx_audio_frame_t frame;

    while (!s_rx_audio_should_stop) {
        if (xQueueReceive(s_rx_audio_queue, &frame, pdMS_TO_TICKS(FSM_RX_AUDIO_POLL_MS)) == pdTRUE) {
            if (audio_io_decode_play(frame.data, frame.len) != 0) {
                ESP_LOGW(TAG, "RX audio decode/play failed: length=%u",
                         (unsigned)frame.len);
            }
        } else {
            /* A missing DATA frame does not mean that the peer released PTT.
             * FHSS channel switching, CRC loss, or recovery can temporarily
             * leave this queue empty while the talkspurt is still active.
             * Keep the I2S clock fed with silence, but remain in RX_AUDIO.
             * Normal termination is driven by the explicit TALKSPURT_END
             * packet; prolonged radio failure is handled by SYNC_LOST. */
            audio_io_write_silence();
        }
    }

    /* speaker를 enable한 RX 태스크가 disable까지 책임진다. 핸들을 먼저 NULL로
     * 만들면 MENU_COMM 진입 코드가 정리를 건너뛰어 다음 RX에서 이미 실행 중인
     * I2S channel을 다시 enable하는 ESP_ERR_INVALID_STATE가 발생한다. */
    audio_io_speaker_disable();
    s_rx_audio_task = NULL;
    vTaskDelete(NULL);
}

/* SYNC_LOST/ERROR처럼 RX_AUDIO를 외부 이벤트로 벗어날 때도 I2S write 중인
 * 태스크를 vTaskDelete()로 끊지 않는다. 최대 queue poll(20ms) 뒤 태스크가
 * speaker를 정상 disable하고 스스로 종료할 때까지 기다린다. */
static void stop_rx_audio_task(void)
{
    if (s_rx_audio_task == NULL) {
        return;
    }

    s_rx_audio_should_stop = true;
    while (s_rx_audio_task != NULL) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    s_rx_audio_should_stop = false;
}

/* BOOT_INIT은 최초 부팅뿐 아니라 ERROR 상태의 EV_RETRY로도 재진입한다
 * (s_transitions의 유일한 탈출구). 아래 하드웨어 init 함수들은 전부 "한 번만
 * 불림"을 가정하고 있어(예: I2C 버스/GPIO ISR을 매번 새로 등록) 재진입 시
 * 그대로 다시 부르면 이미 등록된 리소스에 충돌해 실패할 수 있다 — 그래서
 * s_boot_init_done 플래그로 감싸 최초 1회만 실행되게 한다. */
static bool s_boot_init_done;

/* 상태별 진입 동작. 실제 하드웨어 제어는 각 담당(TODO)이 채운다. */
static void on_enter_boot_init(void)
{
    const bool retrying_after_error = s_boot_init_done;

    char id_hex[DEVICE_ID_LEN * 2 + 1];
    device_id_get_hex(id_hex, sizeof(id_hex));
    ESP_LOGI(TAG, "device id: %s", id_hex);

    if (!s_boot_init_done) {
        display_ui_init();
        status_led_init();
    }

    /* ERROR -> RETRY로 여기 다시 들어왔을 수도 있으니(최초 부팅이 아닌
     * 경우), on_enter_error()에서 켠 빨간 점멸/OLED "ERROR" 표시를 확실히
     * 정리한다 — 둘 다 안 켜져 있었어도 안전(에러 무시). 최초 부팅이든
     * 재진입이든 이 시점엔 이미 display_ui_init()/status_led_init()이
     * 호출된 뒤라(위 분기든 이전 진입이든) s_strip/OLED 핸들이 항상 존재해
     * 안전하다. */
    status_led_stop_error_blink();
    display_ui_clear_status();

    if (!s_boot_init_done) {
        ptt_button_init();
        ptt_button_set_callback(on_ptt_event, NULL);

        rotary_encoder_init();
        rotary_encoder_set_select_callback(on_menu_select, NULL);
        rotary_encoder_set_cursor_callback(on_menu_cursor, NULL);

        audio_codec_init();
        audio_io_init();

        /* Main이 무선 레지스터/홉 시간을 직접 소유하지 않도록 adapter 경계에서
         * CC1101/FHSS 서비스를 시작하고 수신 frame 및 전역 오류만 FSM에 전달한다. */
        const fhss_audio_adapter_config_t fhss_audio_config = {
            .rx_frame_callback = on_fhss_rx_audio_frame,
            .event_callback = on_fhss_audio_event,
            .callback_context = NULL,
        };
        if (!fhss_audio_adapter_init(&fhss_audio_config)) {
            ESP_LOGE(TAG, "FHSS audio adapter initialization failed");
            fsm_post_event(FSM_EVENT_ERROR);
        }

        uint8_t id[DEVICE_ID_LEN] = {0};
        device_id_get(id);
        const ota_client_config_t ota_config = {
            .device_id = ((uint32_t)id[0] << 16U) |
                         ((uint32_t)id[1] << 8U) |
                         (uint32_t)id[2],
            .firmware_version = {
                FIRMWARE_VERSION_MAJOR,
                FIRMWARE_VERSION_MINOR,
                FIRMWARE_VERSION_PATCH,
            },
            .receive_timeout_ms = 3000U,
            .discover_backoff_max_ms = OTA_DISCOVER_BACKOFF_MAX_MS,
            .send_callback = ota_radio_send_callback,
            .event_callback = fsm_ota_event_callback,
            .ota_mode_callback = fsm_ota_mode_callback,
            .random_callback = ota_discovery_random_callback,
            .fhss_activate_callback = ota_fhss_activate_callback,
            .callback_context = NULL,
        };
        if (ota_client_init(&ota_config) != ESP_OK ||
            ota_client_start_consumer() != ESP_OK) {
            ESP_LOGE(TAG, "OTA client initialization failed");
            fsm_post_event(FSM_EVENT_ERROR);
        }

        s_boot_init_done = true;
    }

    /* 최초 부팅의 INIT_DONE은 app_main()이 보낸다. ERROR 복구로 BOOT_INIT에
     * 다시 들어온 경우에는 app_main()이 재실행되지 않으므로 여기서 후속
     * 전이를 예약하지 않으면 BOOT_INIT에 영구 고정된다. */
    if (retrying_after_error) {
        fsm_post_event(FSM_EVENT_INIT_DONE);
    }
}
/* MENU_COMM: 통신 대기(기본 메뉴). TX_AUDIO/RX_AUDIO는 여기서만 나가고
 * 여기로만 돌아오니, 오디오 태스크 정리는 전부 여기서 한다(이전엔
 * on_enter_menu_idle이라는 이름이었음 — 2026-08-11에 개명, 새 MENU_IDLE은
 * 완전히 다른 뮤트 상태). */
static void on_enter_menu_comm(void)
{
    if (!stop_ota_radio()) {
        ESP_LOGW(TAG, "failed to leave OTA radio mode cleanly");
    }
    if (s_tx_audio_task != NULL) {
        stop_tx_audio_task();
#if FHSS_AUDIO_PCM_TEST_ENABLED
        fhss_audio_pcm_test_stats_t stats = {0};
        fhss_audio_pcm_test_get_stats(&stats);
        ESP_LOGI(TAG,
                 "PCM TEST STOP generated=%lu encoded=%lu submitted=%lu "
                 "encode_fail=%lu submit_fail=%lu interval_us[min/max]=%lld/%lld",
                 (unsigned long)stats.generated_frames,
                 (unsigned long)stats.encoded_frames,
                 (unsigned long)stats.submitted_frames,
                 (unsigned long)stats.encode_failures,
                 (unsigned long)stats.submit_failures,
                 (long long)stats.minimum_interval_us,
                 (long long)stats.maximum_interval_us);
#endif
    }
    /* PTT RELEASE 뒤 남은 frame을 마무리하고 CC1101을 시작 채널 RX로 복귀시킨다. */
    if (!fhss_audio_adapter_end_tx()) {
        ESP_LOGW(TAG, "failed to finish FHSS audio TX session cleanly");
    }
    /* on_enter_rx_audio()에서 켠 sky-blue RX 표시등을 여기서 끈다 — 실제
     * 정리(speaker disable 등)는 stop_rx_audio_task()의 협조적 종료가
     * 담당하므로 LED만 별도로 끈다. */
    if (s_rx_audio_task != NULL) {
        status_led_off();
    }
    stop_rx_audio_task();
    /* 재배정(2026-08-17): fsm_post_rx_audio_frame()은 현재 FSM 상태를 보지
     * 않고 무조건 s_rx_audio_queue에 프레임을 채워 넣는다 — RF/FHSS 계층은
     * MENU_IDLE/MENU_OTA 등 COMM이 아닌 메뉴에 있어도 계속 홉핑/추종하며
     * 수신을 이어가기 때문에, 그 동안 상대가 송신하면 이 큐에 오래된
     * 프레임이 그대로 쌓인다. 이걸 안 비우고 COMM으로 돌아오면 다음
     * RX_AUDIO 세션이 새 프레임 대신 이 묵은(순서 깨진) 프레임부터
     * 재생하면서 Speex 디코더 상태가 꼬여 "RX가 안 됨"으로 이어지는
     * 문제가 실기기에서 확인됨(MENU_IDLE 경유 후 재현). COMM 진입 시점에
     * 항상 큐를 비워 다음 세션이 깨끗한 상태로 시작하게 한다. */
    xQueueReset(s_rx_audio_queue);
    display_ui_draw_menu(DISPLAY_UI_MENU_COMM, menu_item_from_rotary(rotary_encoder_get_cursor()));
    display_ui_set_status_scroll("HOLD PTT TO SPEAK");
}

/* MENU_IDLE(뮤트): TX_AUDIO/RX_AUDIO로 들어오는 전이가 없어서(전이표 참고)
 * 오디오 태스크가 실행 중일 수 없다 — 정리할 게 없다. */
static void on_enter_menu_idle(void)
{
    if (!stop_ota_radio()) {
        ESP_LOGW(TAG, "failed to leave OTA radio mode cleanly");
    }
    display_ui_draw_menu(DISPLAY_UI_MENU_IDLE, menu_item_from_rotary(rotary_encoder_get_cursor()));
#ifdef LOOPBACK_ENABLE
    display_ui_set_status_scroll("PRESS PTT TO TEST LOOPBACK");
#else
    display_ui_set_status("MUTED");
#endif
}

static void on_enter_menu_ota(void)
{
    display_ui_draw_menu(DISPLAY_UI_MENU_OTA, menu_item_from_rotary(rotary_encoder_get_cursor()));
    /* 글자 수는 화면 폭(8자)에 다 들어가지만, "대기 중"임을 시각적으로
     * 드러내려고 일부러 흐르는 문구로 표시(display_ui_set_status_scroll()). */
    display_ui_set_status_scroll("STANDBY");
    /* Re-entering from SYNC loss/timeout must tear down the hopping service
     * and re-arm the bootstrap listener on channel 0. */
    if (!stop_ota_radio()) {
        ESP_LOGW(TAG, "failed to reset OTA radio to bootstrap channel");
    }
    if (!start_ota_radio()) {
        ESP_LOGE(TAG, "failed to start OTA channel listener");
        fsm_post_event(FSM_EVENT_ERROR);
    }
}

static void on_enter_ota_fhss_configured(void)
{
    display_ui_set_status_scroll("FHSS READY");
}

static void on_enter_ota_fhss_syncing(void)
{
    display_ui_set_status_scroll("FHSS SYNC");
    if (s_ota_fhss_sync_timer == NULL ||
        xTimerReset(s_ota_fhss_sync_timer, 0U) != pdPASS) {
        ESP_LOGE(TAG, "failed to arm FHSS SYNC timeout");
        fsm_post_event(FSM_EVENT_ERROR);
    }
}

static void on_enter_ota_fhss_ready(void)
{
    display_ui_set_status_scroll("OTA FHSS READY");
}
static void on_enter_tx_audio(void)
{
    display_ui_set_status_animated("TX");

    /* fsm_task 안에서 블로킹으로 끝까지 재생 — 이유는 tx_audio_task 주석 참고. */
    audio_io_speaker_enable();
    audio_io_play_beep();
    audio_io_speaker_disable();

    /* 첫 Speex frame이 RX 역할 서비스로 들어가 유실되지 않도록 캡처 태스크보다
     * 먼저 CC1101/FHSS 서비스를 TX 세션으로 전환한다. */
    if (!fhss_audio_adapter_begin_tx()) {
        ESP_LOGE(TAG, "failed to start FHSS audio TX session");
        fsm_post_event(FSM_EVENT_ERROR);
        return;
    }

#if FHSS_AUDIO_PCM_TEST_ENABLED
    ESP_LOGW(TAG, "PCM TEST MODE: microphone bypassed; real Speex/FHSS/RF path active");
#endif

    /* 스택 8192 — audio_io_capture_encode() -> audio_codec_encode() ->
     * speex_encode_int()(LPC 분석/코드북 탐색) 호출 체인이 4096으론 빠듯해서
     * 여유 있게 늘림. (실기기에서 겪은 재부팅 크래시의 실제 원인은 이게
     * 아니라 스피커 TX DMA 미사용 방치였음 — audio_io.c 주석 참고. 이 스택
     * 크기는 예방적으로 유지.) */
    xTaskCreate(tx_audio_task, "tx_audio", 8192, NULL, tskIDLE_PRIORITY + 3, &s_tx_audio_task);
}
static void on_enter_rx_audio(void)
{
    display_ui_set_status_animated("RX");
    status_led_set_sky_blue_dim();

    /* audio_codec_decode()도 같은 호출 체인 무게라 tx와 동일하게 8192로. */
    audio_io_speaker_enable();
    if (xTaskCreate(rx_audio_task, "rx_audio", 8192, NULL,
                    tskIDLE_PRIORITY + 3, &s_rx_audio_task) != pdPASS) {
        audio_io_speaker_disable();
        s_rx_audio_task = NULL;
        ESP_LOGE(TAG, "failed to create RX audio task");
        fsm_post_event(FSM_EVENT_ERROR);
    }
}
/* OTA_START 수신(OTA_CLIENT_EVENT_STARTED -> FSM_EVENT_OTA_START)으로 여기
 * 진입하는 순간 STANDBY 스크롤 문구를 진행 표시로 바꾼다. 이 시점엔 아직
 * 첫 청크가 안 왔으니 0%로 시작 — 실제 값 갱신은 fsm_ota_event_callback()의
 * OTA_CLIENT_EVENT_PROGRESS 케이스에서. */
static void on_enter_ota_receiving(void)
{
    display_ui_set_status_scroll("RX 0%");
}

/* OTA_COMPLETE 시 ota_writer_finish()가 이미지와 SHA-256을 검증하고 다음 부팅
 * 파티션을 지정한다. 여기서는 최종 END ACK가 전송될 때까지 상태를 유지한다. */
static void on_enter_ota_applying(void)
{
    display_ui_set_status_scroll("VERIFYING");
}
/*
 * EV_ERROR는 전역 전이라(fsm_task() 참고) 어느 상태에서든 여기로 곧장 올 수
 * 있다 — TX_AUDIO/RX_AUDIO 도중이었을 수도 있어서, on_enter_menu_comm()과
 * 동일하게 그 태스크들부터 정리해야 "안전 상태로 정지"가 실제로 보장된다.
 * LED 빨간 점멸(status_led_start_error_blink(), PTT 흰색 고정과 구분되는
 * 패턴)과 OLED 상태 줄("ERROR")로 눈에 보이게 표시. ERROR의 유일한 탈출구인
 * EV_RETRY -> BOOT_INIT에서 둘 다 정리된다(on_enter_boot_init() 참고).
 */
static void on_enter_error(void)
{
    ESP_LOGE(TAG, "entering ERROR state");

    stop_tx_audio_task();
    if (!stop_ota_radio()) {
        ESP_LOGW(TAG, "failed to stop OTA radio while entering ERROR");
    }
    const ota_client_state_t ota_state = ota_client_get_state();
    if ((ota_state == OTA_CLIENT_STATE_RECEIVING ||
         ota_state == OTA_CLIENT_STATE_ERROR) &&
        ota_client_abort() != ESP_OK) {
        ESP_LOGW(TAG, "failed to abort OTA session while entering ERROR");
    }
    /* ERROR에서도 RF TX 세션을 반드시 닫아 producer가 멈춘 뒤 큐가 차며
     * SUBMIT_FAIL이 반복되는 현상을 막고, 상대 수신 가능한 시작 상태로 복귀한다. */
    if (!fhss_audio_adapter_end_tx()) {
        ESP_LOGW(TAG, "failed to stop FHSS audio TX while entering ERROR");
    }
    stop_rx_audio_task();

    status_led_start_error_blink();
    display_ui_set_status("ERROR");
}

static void (*const s_enter_actions[FSM_STATE_COUNT])(void) = {
    [FSM_STATE_BOOT_INIT]     = on_enter_boot_init,
    [FSM_STATE_MENU_COMM]     = on_enter_menu_comm,
    [FSM_STATE_MENU_IDLE]     = on_enter_menu_idle,
    [FSM_STATE_MENU_OTA]      = on_enter_menu_ota,
    [FSM_STATE_OTA_FHSS_CONFIGURED] = on_enter_ota_fhss_configured,
    [FSM_STATE_OTA_FHSS_SYNCING] = on_enter_ota_fhss_syncing,
    [FSM_STATE_OTA_FHSS_READY] = on_enter_ota_fhss_ready,
    [FSM_STATE_TX_AUDIO]      = on_enter_tx_audio,
    [FSM_STATE_RX_AUDIO]      = on_enter_rx_audio,
    [FSM_STATE_OTA_RECEIVING] = on_enter_ota_receiving,
    [FSM_STATE_OTA_APPLYING]  = on_enter_ota_applying,
    [FSM_STATE_ERROR]         = on_enter_error,
};

static void fsm_transition_to(fsm_state_t next_state)
{
    if (next_state == s_state) {
        return;
    }
    if (s_state == FSM_STATE_OTA_FHSS_SYNCING &&
        s_ota_fhss_sync_timer != NULL) {
        (void)xTimerStop(s_ota_fhss_sync_timer, 0U);
    }
    if (s_state == FSM_STATE_OTA_RECEIVING &&
        next_state != FSM_STATE_OTA_RECEIVING &&
        s_ota_fhss_resync_timer != NULL) {
        (void)xTimerStop(s_ota_fhss_resync_timer, 0U);
    }
    ESP_LOGI(TAG, "%s -> %s", s_state_names[s_state], s_state_names[next_state]);
    s_state = next_state;
    if (s_enter_actions[s_state] != NULL) {
        s_enter_actions[s_state]();
    }
}

static void fsm_task(void *arg)
{
    fsm_event_t event;

    /* 부팅 직후 진입 동작 1회 실행 */
    if (s_enter_actions[s_state] != NULL) {
        s_enter_actions[s_state]();
    }

    for (;;) {
        if (xQueueReceive(s_event_queue, &event, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        ESP_LOGD(TAG, "event %s in state %s", s_event_names[event], s_state_names[s_state]);

        /* 전역 전이: 어느 상태에서든 적용 */
        if (event == FSM_EVENT_ERROR && s_state != FSM_STATE_ERROR) {
            fsm_transition_to(FSM_STATE_ERROR);
            continue;
        }
        if (event == FSM_EVENT_SYNC_ACQUIRED &&
            s_state == FSM_STATE_OTA_RECEIVING) {
            if (s_ota_fhss_resync_timer != NULL) {
                (void)xTimerStop(s_ota_fhss_resync_timer, 0U);
            }
            ESP_LOGI(TAG, "OTA FHSS synchronization recovered; session preserved");
            continue;
        }
        if (event == FSM_EVENT_OTA_FHSS_RESYNC_TIMEOUT) {
            if (s_state == FSM_STATE_OTA_RECEIVING) {
                ESP_LOGW(TAG, "OTA FHSS resync grace expired; aborting session");
                if (ota_client_abort() != ESP_OK) {
                    ESP_LOGW(TAG, "failed to abort OTA after resync timeout");
                }
                fsm_transition_to(FSM_STATE_MENU_OTA);
            }
            continue;
        }
        if (event == FSM_EVENT_SYNC_LOST &&
            s_state != FSM_STATE_BOOT_INIT &&
            s_state != FSM_STATE_ERROR) {
            const bool ota_fhss_state =
                s_state == FSM_STATE_OTA_FHSS_CONFIGURED ||
                s_state == FSM_STATE_OTA_FHSS_SYNCING ||
                s_state == FSM_STATE_OTA_FHSS_READY ||
                s_state == FSM_STATE_OTA_RECEIVING;
            if (s_state == FSM_STATE_OTA_RECEIVING) {
                if (s_ota_fhss_resync_timer == NULL ||
                    xTimerReset(s_ota_fhss_resync_timer, 0U) != pdPASS) {
                    ESP_LOGE(TAG, "failed to arm OTA FHSS resync grace");
                    if (ota_client_abort() != ESP_OK) {
                        ESP_LOGW(TAG, "failed to abort OTA after FHSS SYNC loss");
                    }
                    fsm_transition_to(FSM_STATE_MENU_OTA);
                } else {
                    ESP_LOGW(TAG,
                             "OTA FHSS SYNC lost; preserving session for %u ms resync grace",
                             (unsigned)OTA_FHSS_RESYNC_GRACE_MS);
                }
                continue;
            }
            fsm_transition_to(
                ota_fhss_state ? FSM_STATE_MENU_OTA : FSM_STATE_MENU_COMM);
            continue;
        }

        /* 상태별 전이표 조회 */
        bool handled = false;
        for (size_t i = 0; i < sizeof(s_transitions) / sizeof(s_transitions[0]); i++) {
            if (s_transitions[i].state == s_state && s_transitions[i].event == event) {
                fsm_transition_to(s_transitions[i].next_state);
                handled = true;
                break;
            }
        }
        if (!handled) {
            ESP_LOGW(TAG, "unhandled event %s in state %s", s_event_names[event], s_state_names[s_state]);
        }
    }
}

bool fsm_init(void)
{
    s_event_queue = xQueueCreate(16, sizeof(fsm_event_t));
    s_rx_audio_queue = xQueueCreate(FSM_RX_AUDIO_QUEUE_DEPTH, sizeof(fsm_rx_audio_frame_t));
    s_ota_response_done = xSemaphoreCreateBinary();
    s_ota_fhss_sync_timer = xTimerCreate(
        "ota_fhss_sync",
        pdMS_TO_TICKS(OTA_FHSS_SYNC_TIMEOUT_MS),
        pdFALSE,
        NULL,
        ota_fhss_sync_timeout_callback);
    s_ota_fhss_resync_timer = xTimerCreate(
        "ota_fhss_resync",
        pdMS_TO_TICKS(OTA_FHSS_RESYNC_GRACE_MS),
        pdFALSE,
        NULL,
        ota_fhss_resync_timeout_callback);
    if (s_event_queue == NULL || s_rx_audio_queue == NULL ||
        s_ota_response_done == NULL || s_ota_fhss_sync_timer == NULL ||
        s_ota_fhss_resync_timer == NULL) {
        ESP_LOGE(TAG, "failed to allocate FSM queues/semaphore");
        return false;
    }
    if (xTaskCreate(fsm_task, "fsm_task", 4096, NULL,
                    tskIDLE_PRIORITY + 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to create FSM task");
        return false;
    }
    return true;
}

void fsm_post_event(fsm_event_t event)
{
    if (xQueueSend(s_event_queue, &event, 0) != pdTRUE) {
        /* Event loss used to be silent. In particular, a dropped RX_DONE
         * leaves the UI and speaker in RX_AUDIO after TALKSPURT_END. */
        ESP_LOGW(TAG, "event queue full, dropping %s", fsm_event_name(event));
    }
}

bool fsm_post_rx_audio_frame(const uint8_t *data, size_t len)
{
    if (data == NULL || len == 0 || len > FSM_RX_AUDIO_FRAME_MAX_BYTES) {
        return false;
    }

    fsm_rx_audio_frame_t frame;
    frame.len = len;
    memcpy(frame.data, data, len);

    if (xQueueSend(s_rx_audio_queue, &frame, 0) != pdTRUE) {
        ESP_LOGW(TAG, "rx audio queue full, dropping frame");
        return false;
    }

    /* RX_FRAME is a state-entry notification, not one event per 20 ms audio
     * frame. Once RX_AUDIO is active, the dedicated audio queue carries all
     * subsequent frames. Avoid flooding the 16-entry FSM event queue with
     * no-op self-loop events that can hide a critical RX_DONE/SYNC_LOST. */
    if (fsm_get_state() == FSM_STATE_MENU_COMM) {
        fsm_post_event(FSM_EVENT_RX_FRAME);
    }
    return true;
}

fsm_state_t fsm_get_state(void)
{
    return s_state;
}

const char *fsm_state_name(fsm_state_t state)
{
    return (state < FSM_STATE_COUNT) ? s_state_names[state] : "UNKNOWN";
}

const char *fsm_event_name(fsm_event_t event)
{
    return (event < FSM_EVENT_COUNT) ? s_event_names[event] : "UNKNOWN";
}

bool fsm_ota_mode_callback(void *context)
{
    (void)context;
    const fsm_state_t state = fsm_get_state();
    return state == FSM_STATE_MENU_OTA ||
           state == FSM_STATE_OTA_FHSS_CONFIGURED ||
           state == FSM_STATE_OTA_FHSS_SYNCING ||
           state == FSM_STATE_OTA_FHSS_READY;
}

static uint32_t ota_discovery_random_callback(void *context)
{
    (void)context;
    return esp_random();
}

static esp_err_t ota_fhss_activate_callback(
    const ota_fhss_config_fields_t *config,
    void *context)
{
    (void)context;
    return fhss_audio_adapter_activate_ota_fhss(config);
}

void fsm_ota_event_callback(
    ota_client_event_t event,
    uint32_t progress_percent,
    esp_err_t error,
    void *context
)
{
    (void)context;

    switch (event) {
        case OTA_CLIENT_EVENT_FHSS_CONFIG_READY:
            fsm_post_event(FSM_EVENT_FHSS_CONFIG_READY);
            break;
        case OTA_CLIENT_EVENT_FHSS_ACTIVATING:
            fsm_post_event(FSM_EVENT_FHSS_ACTIVATE);
            break;
        case OTA_CLIENT_EVENT_FHSS_ACTIVATE_FAILED:
            ESP_LOGE(TAG, "FHSS activation failed: %s", esp_err_to_name(error));
            fsm_post_event(FSM_EVENT_FHSS_SYNC_TIMEOUT);
            break;
        case OTA_CLIENT_EVENT_STARTED:
            fsm_post_event(FSM_EVENT_OTA_START);
            break;
        case OTA_CLIENT_EVENT_PROGRESS: {
            ESP_LOGI(TAG, "OTA progress: %" PRIu32 "%%", progress_percent);
            /* 실제 청크 개수(n/N)를 표시하려면 ota_client가 progress_percent
             * 말고 총/수신 청크 수도 넘겨줘야 하는데, 지금 콜백 시그니처엔
             * 없어서(팀2 컴포넌트라 임의로 API를 안 늘림) percent로만 표시.
             * 화면 폭 제약도 없어서 진행바 대신 텍스트로 충분(사용자 확인). */
            char status_buf[16];
            snprintf(status_buf, sizeof(status_buf), "RX %" PRIu32 "%%", progress_percent);
            display_ui_set_status_scroll(status_buf);
            break;
        }
        case OTA_CLIENT_EVENT_APPLYING:
            fsm_post_event(FSM_EVENT_OTA_COMPLETE);
            break;
        case OTA_CLIENT_EVENT_COMPLETED:
            /* ota_client emits COMPLETED after esp_ota_set_boot_partition(),
             * before ota_consumer sends the END ACK.  The send callback uses
             * this flag to restart only after that synchronous RF TX succeeds. */
            s_ota_ready_to_reboot = true;
            display_ui_set_status_scroll("RESTARTING");
            break;
        case OTA_CLIENT_EVENT_FAILED:
            ESP_LOGE(TAG, "OTA failed: %s", esp_err_to_name(error));
            if (fsm_get_state() == FSM_STATE_OTA_APPLYING) {
                /* MENU_OTA로 조용히 돌아가기 전에 실패했다는 걸 3초간
                 * 보여준다. FSM_EVENT_OTA_VERIFY_FAIL을 먼저 올려버리면
                 * on_enter_menu_ota()가 곧바로 STANDBY로 덮어써서 실패
                 * 사실이 화면에 전혀 안 남으므로, 메시지를 다 보여준 뒤에
                 * 이벤트를 올리는 순서로 함. 여기서 블로킹되는 건 이
                 * 콜백을 부른 ota_client 컨슈머 태스크지 fsm_task가
                 * 아니라서(다른 이벤트 처리와는 무관), 3초 정도는
                 * 문제없다고 판단(on_enter_tx_audio()의 삐빅음 블로킹과
                 * 같은 이유로 "메시지 다 보여준 뒤 다음 전이"를 보장). */
                display_ui_set_status_scroll("OTA FAILED");
                vTaskDelay(pdMS_TO_TICKS(3000));
                fsm_post_event(FSM_EVENT_OTA_VERIFY_FAIL);
            } else {
                fsm_post_event(FSM_EVENT_ERROR);
            }
            break;
        case OTA_CLIENT_EVENT_ABORTED:
            ESP_LOGW(TAG, "OTA aborted at %" PRIu32 "%%", progress_percent);
            break;
        default:
            break;
    }
}
