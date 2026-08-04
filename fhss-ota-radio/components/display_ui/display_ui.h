#pragma once

#include <stdint.h>

#include "display_ui_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* I2C 버스 초기화 + SSD1306 init sequence 전송, 화면 clear까지 수행.
 * app_main() 등에서 한 번 호출. 실패하면 로그만 남기고 조용히 리턴한다
 * (상태 표시용 보조 장치라 오디오/FHSS 등 핵심 경로를 막지 않기 위함). */
void display_ui_init(void);

/* 프레임버퍼 전체를 지우고 화면에 반영한다. */
void display_ui_clear(void);

/* row: 0 ~ (DISPLAY_UI_TEXT_ROWS-1). 해당 행을 text로 덮어쓰고 즉시 화면에 반영한다.
 * DISPLAY_UI_TEXT_COLS(16자)를 넘는 부분은 잘린다. */
void oled_update_text(uint8_t row, const char *text);

/* printf 스타일 편의 함수. 결과 문자열이 DISPLAY_UI_TEXT_COLS를 넘으면 잘린다. */
void oled_update_text_fmt(uint8_t row, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

#ifdef __cplusplus
}
#endif
