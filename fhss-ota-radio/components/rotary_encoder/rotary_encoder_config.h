#pragma once

#include "driver/gpio.h"

/* 실배선 확정 전 placeholder — 배선 정해지면 이 값만 수정.
 * GPIO5/6/7은 audio_io 마이크(BCLK/WS/SD)가 이미 쓰고 있어서 겹치면 안 됨 —
 * 겹쳤을 때 I2S 클럭 토글이 엔코더 회전으로 오인되는 버그가 있었음(2026-08-10).
 * SW는 원래 GPIO10이었는데, 2026-08-11 브레드보드 재구성으로 audio_io
 * 스피커 SD가 GPIO10을 쓰게 되면서 겹쳐서 GPIO15로 옮김(그때 비게 된 핀). */
#define ROTARY_ENCODER_GPIO_A  GPIO_NUM_8
#define ROTARY_ENCODER_GPIO_B  GPIO_NUM_9
#define ROTARY_ENCODER_GPIO_SW GPIO_NUM_15

/* A/B, SW 공통 폴링 주기(ms). ISR 없이 폴링으로만 처리 (ptt_button과 동일 방식) */
#define ROTARY_ENCODER_POLL_MS 2

/* detent(딸깍 1칸)당 필요한 quadrature step 수. 대부분의 모듈은 4.
 * 메뉴가 한 번에 여러 칸 넘어가거나 반응이 없으면 이 값을 조정할 것 */
#define ROTARY_ENCODER_STEPS_PER_DETENT 4

/* SW(클릭) 디바운스: 1: GND로 눌림(내부 풀업), 0: 3.3V로 눌림(내부 풀다운) */
#define ROTARY_ENCODER_SW_ACTIVE_LOW 1

/* 연속 안정 판정 횟수. 디바운스 시간 = POLL_MS * DEBOUNCE_COUNT (기본 2ms*6=12ms) */
#define ROTARY_ENCODER_SW_DEBOUNCE_COUNT 6

#define ROTARY_ENCODER_TASK_STACK    2048
#define ROTARY_ENCODER_TASK_PRIORITY (tskIDLE_PRIORITY + 3)
