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

#ifdef __cplusplus
}
#endif
