#pragma once

#include "driver/gpio.h"

/* 실배선 확정 전 placeholder — 배선 정해지면 이 값만 수정.
 * 2026-08-11 브레드보드 재구성: 엔코더 모듈 라벨 기준 S1/S2/KEY = A/B/SW.
 * GPIO1은 ptt_button이 쓰고 있어서 겹치면 안 됨 — 그래서 A/B/SW를
 * GPIO2/42/41로 배정(전부 free 확인됨, JTAG용 GPIO41/42지만 JTAG 미사용). */
#define ROTARY_ENCODER_GPIO_A  GPIO_NUM_2  /* S1 */
#define ROTARY_ENCODER_GPIO_B  GPIO_NUM_42 /* S2 */
#define ROTARY_ENCODER_GPIO_SW GPIO_NUM_41 /* KEY */

/* A/B, SW 공통 폴링 주기(ms). ISR 없이 폴링으로만 처리 (ptt_button과 동일 방식).
 * 10ms(=원래 tick 주기)로 한 번 올렸다가 로터리 인식률이 눈에 띄게
 * 떨어져서(빠르게 돌리면 스텝을 놓침) 2ms로 되돌림 — 대신 프로젝트
 * tick rate를 1000Hz로 올려서(sdkconfig.defaults의 CONFIG_FREERTOS_HZ=1000
 * 참고) 2ms가 실제로 2틱이 되게 함(2026-08-12). 아래 #error 가드가 이
 * 조합이 다시 깨지면(예: 누가 tick rate를 도로 낮추면) 조용히 busy-loop이
 * 되는 대신 빌드를 막아준다. */
#define ROTARY_ENCODER_POLL_MS 2

#if (ROTARY_ENCODER_POLL_MS * CONFIG_FREERTOS_HZ) < 1000
#error "ROTARY_ENCODER_POLL_MS가 현재 CONFIG_FREERTOS_HZ에서 0틱이 됨(vTaskDelay(0) busy-loop). ROTARY_ENCODER_POLL_MS를 올리거나 CONFIG_FREERTOS_HZ를 1000으로 맞출 것 — troubleshoot/task_wdt-poll_ms_zero_tick_busyloop.md 참고."
#endif

/* detent(딸깍 1칸)당 필요한 quadrature step 수. 대부분의 모듈은 4.
 * 메뉴가 한 번에 여러 칸 넘어가거나 반응이 없으면 이 값을 조정할 것 */
#define ROTARY_ENCODER_STEPS_PER_DETENT 4

/* SW(클릭) 디바운스: 1: GND로 눌림(내부 풀업), 0: 3.3V로 눌림(내부 풀다운) */
#define ROTARY_ENCODER_SW_ACTIVE_LOW 1

/* 연속 안정 판정 횟수. 디바운스 시간 = POLL_MS * DEBOUNCE_COUNT (2ms*6=12ms) */
#define ROTARY_ENCODER_SW_DEBOUNCE_COUNT 6

#define ROTARY_ENCODER_TASK_STACK    2048
#define ROTARY_ENCODER_TASK_PRIORITY (tskIDLE_PRIORITY + 3)
