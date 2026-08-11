#pragma once

#include "driver/gpio.h"

/* 실배선 확정 전 placeholder — 배선 정해지면 이 값만 수정 */
#define PTT_BUTTON_GPIO GPIO_NUM_1

/* 1: 버튼이 GND로 눌림(내부 풀업 사용), 0: 버튼이 3.3V로 눌림(내부 풀다운 사용) */
#define PTT_BUTTON_ACTIVE_LOW 1

/* 디바운스 폴링 주기(ms) */
#define PTT_BUTTON_POLL_MS 5

/* 연속 안정 판정 횟수. 디바운스 시간 = POLL_MS * DEBOUNCE_COUNT (기본 5ms*6=30ms) */
#define PTT_BUTTON_DEBOUNCE_COUNT 6

#define PTT_BUTTON_TASK_STACK    2048
#define PTT_BUTTON_TASK_PRIORITY (tskIDLE_PRIORITY + 3)
