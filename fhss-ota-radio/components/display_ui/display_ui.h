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

/* OLED header에 표시할 펌웨어 버전을 설정한다. 화면 방향/상태 API와 독립적이다. */
void display_ui_set_firmware_version(uint8_t major, uint8_t minor, uint8_t patch);

/* row: 0 ~ (DISPLAY_UI_TEXT_ROWS-1). 해당 행을 text로 덮어쓰고 즉시 화면에 반영한다.
 * DISPLAY_UI_TEXT_COLS(16자)를 넘는 부분은 잘린다.
 * 참고: 물리(가로) 좌표 기준 API라 display_ui_draw_menu()의 회전된 화면과는
 * 같이 쓰지 않는다 — 지금은 메뉴 화면이 draw_menu()로 완전히 대체해서 그린다. */
void oled_update_text(uint8_t row, const char *text);

/* printf 스타일 편의 함수. 결과 문자열이 DISPLAY_UI_TEXT_COLS를 넘으면 잘린다. */
void oled_update_text_fmt(uint8_t row, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/* 메뉴 화면의 세 항목. 화면에는 위에서부터 COMM/IDLE/OTA 순서로 그려진다. */
typedef enum {
    DISPLAY_UI_MENU_COMM = 0,
    DISPLAY_UI_MENU_IDLE,
    DISPLAY_UI_MENU_OTA,
    DISPLAY_UI_MENU_COUNT,
} display_ui_menu_item_t;

/*
 * 배선은 그대로 두고(SDA/SCL 안 바뀜) 화면 내용만 좌측으로 90도 돌려서(세로,
 * 64x128 논리 캔버스) 그리는 메뉴 화면. SSD1306엔 진짜 90도 회전 명령이
 * 없어서(A0/A1, C0/C8은 0/180도 반전만 지원) 프레임버퍼에 좌표 변환으로
 * 직접 그린 뒤 통째로 flush한다.
 *
 * 매 호출마다 화면 전체를 지우고 다시 그린다:
 *   - 좌상단에 작게 "mode" 라벨
 *   - 그 아래 COMM/IDLE/OTA 세 박스(간격 있음), 기존 8x8 폰트를 2배 확대해
 *     굵고 크게 표시
 *   - selected 항목: 배경/글자색 반전(채워진 흰 배경 + 검은 글자)
 *   - hovered 항목: 흰색(또는 selected와 겹치면 반전된) 테두리 박스로 표시
 *
 * selected와 hovered는 같은 항목일 수 있다(그 경우 반전 배경 위에 테두리).
 */
void display_ui_draw_menu(display_ui_menu_item_t selected, display_ui_menu_item_t hovered);

/*
 * 메뉴 3항목 바로 아래 상태 메시지 한 줄을 정적 텍스트로 표시한다(scale1,
 * 논리 너비 64px 기준 최대 8자 — 넘는 부분은 그냥 잘려 그려짐). 애니메이션
 * 중이었으면 멈추고 이 텍스트로 덮어쓴다. 가장 최근 display_ui_draw_menu()
 * 호출의 선택/커서 상태는 그대로 유지된 채 상태 줄만 바뀐다.
 */
void display_ui_set_status(const char *text);

/*
 * base 뒤에 마침표(.)가 0~3개, 250ms마다 하나씩 늘었다가 다시 0개로
 * 돌아가는 로딩 애니메이션을 시작한다(내부 esp_timer 주기 타이머로 자체
 * 갱신 — 호출자가 계속 다시 부를 필요 없음). base는 마침표 3개를 더해도
 * 8자를 넘지 않게 5자 이하로 줄 것(예: "TX", "RX"). display_ui_set_status(),
 * display_ui_set_status_scroll() 또는 display_ui_clear_status() 호출 시
 * 자동으로 멈춘다.
 */
void display_ui_set_status_animated(const char *base);

/*
 * text를 왼쪽으로 천천히 흐르는 문구(marquee)로 표시한다(내부 esp_timer로
 * 자체 갱신). display_ui_set_status()와 달리 8자 제한이 없어 더 긴 안내
 * 문구를 담을 수 있고(버퍼 한도 STATUS_TEXT_MAX_LEN, display_ui.c 참고),
 * 화면 폭보다 짧아도(예: "STANDBY") 끊김 없이 계속 흐른다 — 짧은 문구를
 * "동작 중" 표시로도 쓸 수 있게 하기 위함. 갱신 주기는 일부러 느리게(400ms)
 * 잡혀 있다 — 너무 자주 다시 그리면 esp_timer/I2C flush 부담이 늘어서다.
 * display_ui_set_status() 또는 display_ui_clear_status() 호출 시 멈춘다.
 */
void display_ui_set_status_scroll(const char *text);

/* 상태 메시지 영역을 비운다(애니메이션/스크롤 중이었으면 멈춤). */
void display_ui_clear_status(void);

#ifdef __cplusplus
}
#endif
