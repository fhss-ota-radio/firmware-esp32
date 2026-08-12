#pragma once

#include "driver/gpio.h"

/* 실배선 확정 전 placeholder — 배선 정해지면 이 값만 수정 */
#define PTT_BUTTON_GPIO GPIO_NUM_1

/* 1: 버튼이 GND로 눌림(내부 풀업 사용), 0: 버튼이 3.3V로 눌림(내부 풀다운 사용) */
#define PTT_BUTTON_ACTIVE_LOW 1

/* 디바운스 폴링 주기(ms). 프로젝트 tick rate를 1000Hz로 올려서(sdkconfig.defaults의
 * CONFIG_FREERTOS_HZ=1000, rotary_encoder_config.h 주석 참고) 원래 값(5ms)으로
 * 되돌림 — 100Hz였을 땐 0틱(busy-loop, 2026-08-12 실기기 확인)이었음. 아래
 * #error 가드가 이 조합이 다시 깨지면 조용히 busy-loop이 되는 대신 빌드를
 * 막아준다. */
#define PTT_BUTTON_POLL_MS 5

#if (PTT_BUTTON_POLL_MS * CONFIG_FREERTOS_HZ) < 1000
#error "PTT_BUTTON_POLL_MS가 현재 CONFIG_FREERTOS_HZ에서 0틱이 됨(vTaskDelay(0) busy-loop). PTT_BUTTON_POLL_MS를 올리거나 CONFIG_FREERTOS_HZ를 1000으로 맞출 것 — troubleshoot/task_wdt-poll_ms_zero_tick_busyloop.md 참고."
#endif

/* 연속 안정 판정 횟수. 디바운스 시간 = POLL_MS * DEBOUNCE_COUNT (5ms*6=30ms) */
#define PTT_BUTTON_DEBOUNCE_COUNT 6

#define PTT_BUTTON_TASK_STACK    2048
#define PTT_BUTTON_TASK_PRIORITY (tskIDLE_PRIORITY + 3)
