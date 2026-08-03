#include "fsm.h"

#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "fsm";

static QueueHandle_t s_event_queue;
static fsm_state_t s_state = FSM_STATE_BOOT_INIT;

/* 상태별 이름/이벤트별 이름: 로그 및 OLED 표시용 */
static const char *s_state_names[FSM_STATE_COUNT] = {
    [FSM_STATE_BOOT_INIT]     = "BOOT_INIT",
    [FSM_STATE_FHSS_SYNC]     = "FHSS_SYNC",
    [FSM_STATE_IDLE]          = "IDLE",
    [FSM_STATE_TX_AUDIO]      = "TX_AUDIO",
    [FSM_STATE_RX_AUDIO]      = "RX_AUDIO",
    [FSM_STATE_OTA_RECEIVING] = "OTA_RECEIVING",
    [FSM_STATE_OTA_APPLYING]  = "OTA_APPLYING",
    [FSM_STATE_ERROR]         = "ERROR",
};

static const char *s_event_names[FSM_EVENT_COUNT] = {
    [FSM_EVENT_INIT_DONE]      = "INIT_DONE",
    [FSM_EVENT_SYNC_ACQUIRED]  = "SYNC_ACQUIRED",
    [FSM_EVENT_SYNC_LOST]      = "SYNC_LOST",
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
 * 주의: 이 표에는 FHSS 홉 타이밍 보정이 없다 — 그것은 이 FSM의 상태가 아니라
 * nRF24L01 수신 드라이버(팀5)가 매 패킷 검증 성공 시 그 자리에서 홉 타이머를
 * 보정하는 이벤트 기반 로직이다. 정상 수신은 이벤트로 올라오지 않고, 연속
 * 수신 실패로 완전 동기 상실이 판정될 때만 EV_SYNC_LOST가 올라온다. 이 FSM은
 * 그 결과인 EV_SYNC_ACQUIRED / EV_SYNC_LOST 두 이벤트만 소비한다.
 */
typedef struct {
    fsm_state_t state;
    fsm_event_t event;
    fsm_state_t next_state;
} fsm_transition_t;

static const fsm_transition_t s_transitions[] = {
    { FSM_STATE_BOOT_INIT,     FSM_EVENT_INIT_DONE,      FSM_STATE_FHSS_SYNC },
    { FSM_STATE_FHSS_SYNC,     FSM_EVENT_SYNC_ACQUIRED,  FSM_STATE_IDLE },

    { FSM_STATE_IDLE,          FSM_EVENT_PTT_PRESS,      FSM_STATE_TX_AUDIO },
    { FSM_STATE_IDLE,          FSM_EVENT_RX_FRAME,       FSM_STATE_RX_AUDIO },
    { FSM_STATE_IDLE,          FSM_EVENT_OTA_START,      FSM_STATE_OTA_RECEIVING },

    { FSM_STATE_TX_AUDIO,      FSM_EVENT_PTT_RELEASE,    FSM_STATE_IDLE },
    { FSM_STATE_TX_AUDIO,      FSM_EVENT_OTA_START,      FSM_STATE_OTA_RECEIVING },

    { FSM_STATE_RX_AUDIO,      FSM_EVENT_RX_DONE,        FSM_STATE_IDLE },
    { FSM_STATE_RX_AUDIO,      FSM_EVENT_OTA_START,      FSM_STATE_OTA_RECEIVING },

    { FSM_STATE_OTA_RECEIVING, FSM_EVENT_OTA_CHUNK,      FSM_STATE_OTA_RECEIVING },
    { FSM_STATE_OTA_RECEIVING, FSM_EVENT_OTA_COMPLETE,   FSM_STATE_OTA_APPLYING },

    { FSM_STATE_OTA_APPLYING,  FSM_EVENT_OTA_VERIFY_OK,   FSM_STATE_BOOT_INIT },
    { FSM_STATE_OTA_APPLYING,  FSM_EVENT_OTA_VERIFY_FAIL, FSM_STATE_IDLE },

    { FSM_STATE_ERROR,         FSM_EVENT_RETRY,          FSM_STATE_BOOT_INIT },
};

/* 상태별 진입 동작. 실제 하드웨어 제어는 각 담당(TODO)이 채운다. */
static void on_enter_boot_init(void)     { /* TODO(팀1/2): I2S, OLED, SPI(nRF24L01/CC1101), PTT GPIO 초기화 */ }
static void on_enter_fhss_sync(void)     { /* TODO(팀5): 호핑 시퀀스 동기화 시작 */ }
static void on_enter_idle(void)          { /* TODO(팀1): OLED "대기" 표시 */ }
static void on_enter_tx_audio(void)      { /* TODO(팀1/2): 마이크 캡처 + Opus 인코딩 시작 */ }
static void on_enter_rx_audio(void)      { /* TODO(팀1/2): Opus 디코딩 + 스피커 재생 시작 */ }
static void on_enter_ota_receiving(void) { /* TODO(팀2): OTA 수신 버퍼 초기화, 음성 태스크 일시 중단 */ }
static void on_enter_ota_applying(void)  { /* TODO(팀2): 이미지 검증 및 OTA 파티션 기록 */ }
static void on_enter_error(void)         { /* TODO(팀1/PM): 오류 로깅, 안전 상태로 정지 */ }

static void (*const s_enter_actions[FSM_STATE_COUNT])(void) = {
    [FSM_STATE_BOOT_INIT]     = on_enter_boot_init,
    [FSM_STATE_FHSS_SYNC]     = on_enter_fhss_sync,
    [FSM_STATE_IDLE]          = on_enter_idle,
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
        if (event == FSM_EVENT_SYNC_LOST &&
            s_state != FSM_STATE_FHSS_SYNC &&
            s_state != FSM_STATE_BOOT_INIT &&
            s_state != FSM_STATE_ERROR) {
            fsm_transition_to(FSM_STATE_FHSS_SYNC);
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

void fsm_init(void)
{
    s_event_queue = xQueueCreate(16, sizeof(fsm_event_t));
    xTaskCreate(fsm_task, "fsm_task", 4096, NULL, tskIDLE_PRIORITY + 3, NULL);
}

void fsm_post_event(fsm_event_t event)
{
    xQueueSend(s_event_queue, &event, 0);
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
