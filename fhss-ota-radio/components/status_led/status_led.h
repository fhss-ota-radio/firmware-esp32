#pragma once

#include "status_led_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 온보드 WS2812 RGB LED(RMT 기반) 초기화. app_main 등에서 한 번 호출. */
void status_led_init(void);

/* 흰색을 STATUS_LED_DIM_BRIGHTNESS 밝기로 켠다. */
void status_led_set_white_dim(void);

/* LED를 끈다. */
void status_led_off(void);

/*
 * 빨간색을 STATUS_LED_DIM_BRIGHTNESS 밝기로 STATUS_LED_BLINK_INTERVAL_MS
 * 주기로 점멸시킨다(내부 esp_timer 주기 타이머로 자체 반복). ERROR 상태
 * 표시용 — PTT의 흰색 고정 점등/꺼짐과 겹치지 않는 패턴으로 구분했다.
 * 이미 점멸 중이면 다시 처음부터(켜진 상태로) 재시작한다.
 */
void status_led_start_error_blink(void);

/* 점멸을 멈추고 LED를 끈다. 점멸 중이 아니었어도 안전(에러 무시). */
void status_led_stop_error_blink(void);

#ifdef __cplusplus
}
#endif
