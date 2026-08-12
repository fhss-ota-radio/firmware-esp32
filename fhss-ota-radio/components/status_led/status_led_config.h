#pragma once

/* ESP32-S3 devkit 온보드 WS2812 RGB LED. 보드 실물 기준(GPIO38) — 다른 보드로
 * 바뀌면 이 값만 수정 (일부 리비전은 GPIO48을 씀). */
#define STATUS_LED_GPIO     38
#define STATUS_LED_MAX_LEDS 1

/* "약하게" 밝기 — 0~255 중 낮은 값. 너무 밝으면 이 값만 낮추면 됨. */
#define STATUS_LED_DIM_BRIGHTNESS 8

/* ERROR 상태 표시용 빨간 점멸 주기(ms, on/off 각각 이 시간). PTT의 흰색
 * 고정 점등과 구분되게 눈에 띄는 패턴으로. */
#define STATUS_LED_BLINK_INTERVAL_MS 300
