#pragma once

#include <stdbool.h>

#include "rotary_encoder_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 메뉴 커서 값. docs/fsm-design.md 기준 순회 순서: IDLE -> OTA -> IDLE (시계방향 = 아래로).
 * 이 컴포넌트는 FSM을 모른다 — select 콜백에서 상위 코드가 fsm_post_event()로 매핑한다.
 */
typedef enum {
    ROTARY_ENCODER_MENU_IDLE = 0,
    ROTARY_ENCODER_MENU_OTA,
    ROTARY_ENCODER_MENU_COUNT,
} rotary_encoder_menu_t;

/* 회전으로 커서만 이동했을 때 호출 (표시 갱신용, FSM 이벤트 아님). */
typedef void (*rotary_encoder_cursor_cb_t)(rotary_encoder_menu_t cursor, void *ctx);

/* SW 클릭으로 확정했을 때 호출 (그 시점 커서가 selected로 전달됨). */
typedef void (*rotary_encoder_select_cb_t)(rotary_encoder_menu_t selected, void *ctx);

/* GPIO 설정 + 폴링 태스크 생성. app_main 등에서 한 번 호출. */
void rotary_encoder_init(void);

void rotary_encoder_set_cursor_callback(rotary_encoder_cursor_cb_t cb, void *ctx);
void rotary_encoder_set_select_callback(rotary_encoder_select_cb_t cb, void *ctx);

/* 현재 커서 폴링 조회 (콜백 대신/추가로 사용 가능). */
rotary_encoder_menu_t rotary_encoder_get_cursor(void);

#ifdef __cplusplus
}
#endif
