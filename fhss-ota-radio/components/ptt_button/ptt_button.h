#pragma once

#include <stdbool.h>

#include "ptt_button_config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*ptt_button_cb_t)(bool pressed, void *ctx);

/* GPIO 설정 + 디바운스 폴링 태스크 생성. app_main 등에서 한 번 호출. */
void ptt_button_init(void);

/* 눌림/뗌이 디바운스 확정될 때마다 호출될 콜백 등록 (교체 가능, NULL이면 해제).
 * FSM 등 상위 로직 연결은 이 콜백 안에서 하면 된다 — 이 컴포넌트는 FSM을 모른다. */
void ptt_button_set_callback(ptt_button_cb_t cb, void *ctx);

/* 현재 디바운스 확정된 상태를 폴링으로 조회 (콜백 대신/추가로 사용 가능). */
bool ptt_button_is_pressed(void);

#ifdef __cplusplus
}
#endif
