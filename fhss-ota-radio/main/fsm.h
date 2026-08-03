#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 상태/이벤트 정의 및 전이표는 docs/fsm-design.md 참고 */

/*
 * FSM_STATE_FHSS_SYNC는 홉 동기 "획득/재획득" 전용 상태다 (최초 부팅 또는
 * 동기 완전 상실 시에만 진입). 동기가 잡힌 뒤의 홉 추종·드리프트 보정은
 * 이 상태기계와 별개로 항상 병행 실행되는 FHSS 태스크(팀5, fhss_link)가
 * 담당하며, IDLE/TX_AUDIO/RX_AUDIO/OTA_* 어떤 상태에서도 끊기지 않는다.
 * 자세한 내용은 docs/fsm-design.md §1.1 참고.
 */
typedef enum {
    FSM_STATE_BOOT_INIT = 0,
    FSM_STATE_FHSS_SYNC,
    FSM_STATE_IDLE,
    FSM_STATE_TX_AUDIO,
    FSM_STATE_RX_AUDIO,
    FSM_STATE_OTA_RECEIVING,
    FSM_STATE_OTA_APPLYING,
    FSM_STATE_ERROR,
    FSM_STATE_COUNT,
} fsm_state_t;

typedef enum {
    FSM_EVENT_INIT_DONE = 0,
    FSM_EVENT_SYNC_ACQUIRED,
    FSM_EVENT_SYNC_LOST,
    FSM_EVENT_PTT_PRESS,
    FSM_EVENT_PTT_RELEASE,
    FSM_EVENT_RX_FRAME,
    FSM_EVENT_RX_DONE,
    FSM_EVENT_OTA_START,
    FSM_EVENT_OTA_CHUNK,
    FSM_EVENT_OTA_COMPLETE,
    FSM_EVENT_OTA_VERIFY_OK,
    FSM_EVENT_OTA_VERIFY_FAIL,
    FSM_EVENT_ERROR,
    FSM_EVENT_RETRY,
    FSM_EVENT_COUNT,
} fsm_event_t;

/* FSM 태스크와 이벤트 큐를 생성한다. app_main()에서 한 번 호출. */
void fsm_init(void);

/* 다른 태스크/ISR에서 이벤트를 큐에 넣는다 (ISR에서는 안전하지 않음, 디퍼드 처리 필요). */
void fsm_post_event(fsm_event_t event);

/* 현재 상태 조회 (디버그/OLED 표시용). */
fsm_state_t fsm_get_state(void);

const char *fsm_state_name(fsm_state_t state);
const char *fsm_event_name(fsm_event_t event);

#ifdef __cplusplus
}
#endif
