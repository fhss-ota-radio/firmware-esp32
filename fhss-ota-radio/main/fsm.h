#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 상태/이벤트 정의 및 전이표는 docs/fsm-design.md 참고 */

/*
 * FSM_STATE_FHSS_SYNC는 홉 동기 "획득/재획득" 전용 상태다 (최초 부팅 또는
 * 연속 수신 실패로 동기를 완전히 잃었을 때만 진입). 동기가 잡힌 뒤의 타이밍
 * 유지는 별도 태스크가 계속 도는 방식이 아니라, CC1101 수신 드라이버가
 * 매 패킷 검증 성공 시 그 자리에서 홉 타이머를 보정하는 이벤트 기반 방식이다
 * (팀5 담당). 정상 수신은 FSM에 이벤트로 올라오지 않으며, 연속 N회 검증
 * 실패로 완전 동기 상실이 판정될 때만 FSM_EVENT_SYNC_LOST가 올라온다.
 *
 * 음성(FHSS)과 OTA는 같은 CC1101 라디오를 공유한다(단일 반이중 트랜시버).
 * OTA_RECEIVING 동안은 CC1101이 의도적으로 음성 호핑을 이탈하므로, 그 사이의
 * 미수신은 동기 상실로 세지 않는다.
 *
 * FSM_STATE_MENU_IDLE(음성, 기본)과 FSM_STATE_MENU_OTA(OTA 대기)는 수신 패킷의
 * 해석 자체를 게이팅한다 — MENU_IDLE에서 받은 패킷은 음성, MENU_OTA에서 받은
 * 패킷은 펌웨어 청크로 간주한다. 메뉴 전환(FSM_EVENT_MENU_SELECT_IDLE/OTA)은
 * 로터리 엔코더 클릭으로만 발생하며, 이 두 메뉴 상태 사이에서만 정의되어 있어
 * TX_AUDIO/RX_AUDIO/OTA_RECEIVING/OTA_APPLYING 중에는 전이가 없다 = 메뉴 변경 불가.
 * 회전(커서 이동)은 FSM 이벤트가 아니라 display_ui 로컬 상태다.
 * 자세한 내용은 docs/fsm-design.md §1, §1.1 참고.
 */
typedef enum {
    FSM_STATE_BOOT_INIT = 0,
    FSM_STATE_FHSS_SYNC,
    FSM_STATE_MENU_IDLE,
    FSM_STATE_MENU_OTA,
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
    FSM_EVENT_MENU_SELECT_IDLE,
    FSM_EVENT_MENU_SELECT_OTA,
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

/*
 * 수신된 오디오 프레임 데이터를 FSM에 전달한다. fsm_post_event(FSM_EVENT_RX_FRAME)와
 * 달리 데이터(len 바이트)를 같이 옮긴다 — 내부적으로 별도 큐에 복사해두고
 * FSM_EVENT_RX_FRAME도 함께 올리므로, 이 함수를 호출했으면 fsm_post_event()를
 * 따로 또 부를 필요는 없다.
 *
 * rf_transport/fhss_core가 수신 프레임을 검증한 뒤 여기로 넘기는 용도로 설계됨
 * (아직 그 컴포넌트가 없어 실제 호출자는 없음 — 인터페이스만 먼저 정의).
 * ISR에서는 안전하지 않음(fsm_post_event와 동일 — 디퍼드 처리 필요).
 *
 * len이 버퍼 한도를 넘거나 내부 큐가 가득 차 있으면 false를 반환하고 프레임을
 * 버린다(호출자가 이를 드롭/재시도 여부 판단에 사용할 수 있음). 버퍼 한도는
 * 현재 audio_codec 기준 AUDIO_CODEC_MAX_ENCODED_BYTES(64바이트)다.
 */
bool fsm_post_rx_audio_frame(const uint8_t *data, size_t len);

/* 현재 상태 조회 (디버그/OLED 표시용). */
fsm_state_t fsm_get_state(void);

const char *fsm_state_name(fsm_state_t state);
const char *fsm_event_name(fsm_event_t event);

#ifdef __cplusplus
}
#endif
