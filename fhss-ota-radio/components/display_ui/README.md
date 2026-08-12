# display_ui

0.96" I2C OLED(SSD1306, 128x64) 상태 표시 컴포넌트. `main/fsm.c`에 연결됨.

## 구성

| 파일 | 내용 |
|---|---|
| `display_ui_config.h` | 핀/주소/해상도 설정. **하드웨어 바뀌면 여기만 수정** |
| `display_ui.h` | 공개 API |
| `display_ui.c` | SSD1306 드라이버(`driver/i2c_master.h` 기반) + 회전/메뉴 화면 렌더링 |
| `font8x8_basic.h` | 8x8 비트맵 폰트, public domain ([dhepper/font8x8](https://github.com/dhepper/font8x8)) |

## 메뉴 화면 (2026-08-11 재설계)

배선(SDA/SCL)은 그대로 두고 **화면 내용만 좌측으로 90도 돌려서**(세로,
논리 캔버스 64x128) COMM/IDLE/OTA 3항목 메뉴를 그린다. SSD1306엔 진짜
90도 회전 명령이 없어서(A0/A1, C0/C8은 0/180도 반전만 지원) 프레임버퍼에
좌표 변환으로 직접 그린 뒤 8페이지 전체를 매번 flush한다.

```c
#include "display_ui.h"

display_ui_init(); // 부팅 시 1회

// selected: 현재 확정된 FSM 메뉴 상태 (배경/글자색 반전으로 표시)
// hovered:  로터리 커서가 지금 가리키는 항목 (흰 테두리로 표시, selected와 겹치면 검은 테두리)
display_ui_draw_menu(DISPLAY_UI_MENU_COMM, DISPLAY_UI_MENU_IDLE);
```

- 좌상단에 작은 "mode" 라벨, 그 아래 COMM/IDLE/OTA 세 박스(간격 있음), 그 아래 상태 메시지 한 줄(아래 참고)
- 각 박스 텍스트는 기존 8x8 폰트를 2배 확대(픽셀 더블링)해서 굵고 크게 표시 — 새 폰트 에셋 없이 구현
- `selected`/`hovered`는 같은 항목일 수 있음(반전 배경 위에 테두리)
- 호출마다 프레임버퍼 전체를 지우고 다시 그리므로, 회전/선택 시마다 전체 화면이 다시 그려짐(부분 갱신 아님)

**2026-08-12(2차) 비주얼 다듬기**: 항목 배경/테두리를 각진 사각형에서 **둥근 사각형**(`MENU_ITEM_RADIUS`=3px, 4모서리만 원으로 깎음)으로 바꾸고, 헤더 아래·상태 영역 위에 얇은 구분선(rule)을 넣어 구획을 분명히 함 — 예전엔 항목 3개가 화면에 꽉 차서 딱딱해 보였음. 항목 자체의 좌우 폭(0~64, 전체 폭)은 그대로 뒀음 — `COMM`/`IDLE`이 scale2(글자당 16px)에서 정확히 4×16=64px을 채워야 해서 좌우 여백을 주면 텍스트가 카드 밖으로 삐져나감(실측 확인) — 구분선만 좌우로 살짝 인셋(`RULE_X`=4)해서 카드 느낌을 냄.

## 상태 메시지 (2026-08-12 추가)

메뉴 3항목 바로 아래에 "지금 이 모드에서 뭘 하고 있는지"를 짧게 보여주는 한 줄(scale1, 최대 8자 — 논리 너비 64px 한계).

```c
display_ui_set_status("HOLD PTT");                    // 정적 텍스트(최대 8자, 넘으면 잘림)
display_ui_set_status_animated("TX");                 // base + 마침표 0~3개가 250ms마다 늘어남(내부 타이머로 자동 반복)
display_ui_set_status_scroll("HOLD PTT TO SPEAK");    // 왼쪽으로 천천히 흐르는 문구(내부 타이머, 400ms/1px)
display_ui_clear_status();                            // 비우기(애니메이션/스크롤 중이면 멈춤)
```

- `display_ui_draw_menu()`를 다시 불러도(로터리 커서 이동 등) 마지막 상태 텍스트는 유지됨
- 애니메이션(`set_status_animated`) base는 마침표 3개를 더해도 8자를 안 넘게 5자 이하로 (`main/fsm.c`: TX_AUDIO="TX", RX_AUDIO="RX")
- 흐르는 문구(`set_status_scroll`)는 8자 제한이 없음 — 폭보다 긴 문구는 왼쪽으로 흐르고, 짧아도(예: "STANDBY") 끊김 없이 계속 흘러서 "동작 중"임을 겸해 표현함. 픽셀 단위가 아니라 **글자 하나 폭씩**(200ms마다 8px, `STATUS_SCROLL_STEP_PX`) 통째로 옮겨서 항상 글자 경계에 맞춰 이동함 — 어중간하게 잘린 글자가 화면에 걸치지 않아 깔끔함. 갱신 주기는 200ms(초당 5회)까지 낮춰봄 — render_screen()이 화면 전체를 다시 그리고 8페이지를 I2C로 flush하는 비용이 있어 이보다 더 빠르게 하려면 주기를 더 줄이기보다 `STATUS_SCROLL_STEP_PX`를 올리는 쪽을 우선 고려(display_ui.c `STATUS_SCROLL_*` 주석 참고)
- `main/fsm.c`의 `on_enter_*` 함수들에 연결됨: MENU_COMM="HOLD PTT TO SPEAK"(흐름), TX_AUDIO="TX..."(점), RX_AUDIO="RX..."(점), MENU_IDLE="MUTED"(`LOOPBACK_ENABLE` 켜면 "PRESS PTT TO TEST LOOPBACK", 흐름), MENU_OTA="STANDBY"(흐름)
- 상태 영역 높이(28px)는 텍스트 한 줄보다 넉넉히 잡아뒀음 — 나중에 OTA 진행률 바를 추가할 여유(TODO, 팀2)

## 레거시 텍스트 API (물리/가로 좌표, 회전 미적용)

```c
oled_update_text(0, "CH 3  BAT 82%");        // row 0에 텍스트 표시
oled_update_text_fmt(1, "TX %d dBm", -42);   // printf 스타일
display_ui_clear();                          // 전체 지우기
```

- `row` 범위: `0 ~ DISPLAY_UI_TEXT_ROWS-1` (128x64 기준 0~7)
- 한 줄 최대 `DISPLAY_UI_TEXT_COLS`(16)자, 초과분은 잘림
- 지금은 아무도 안 씀(메뉴 화면은 `display_ui_draw_menu()`가 전담) — 범용 상태 텍스트가 필요해질 때를 위해 남겨둠. `display_ui_draw_menu()`와 같이 쓰면 좌표계가 달라 화면이 뒤섞이니 섞어 쓰지 말 것

## 하드웨어 배선 확정 시 수정할 것 (`display_ui_config.h`)

- `DISPLAY_UI_I2C_SDA_GPIO` / `DISPLAY_UI_I2C_SCL_GPIO` — 현재 SDA=GPIO21, SCL=GPIO20(2026-08-11 브레드보드 재구성 배선). SCL은 native USB D+ 핀 겸용이라 USB-OTG 안 쓰면 문제없음. 최종 배선 확정 전까지는 바뀔 수 있음, 다른 보드/배선이면 이 값만 수정
- `DISPLAY_UI_I2C_ADDR` — 기본 `0x3C`. 모듈에 따라 `0x3D`인 경우도 있음, 안 켜지면 제일 먼저 의심
- `DISPLAY_UI_I2C_PORT` — 다른 I2C 장치와 버스 공유 시 포트 번호 조정
- `DISPLAY_UI_WIDTH` / `HEIGHT` — 128x64 아닌 다른 크기 모듈로 바뀌면 수정(회전 매핑도 이 값들을 그대로 참조하므로 따로 안 고쳐도 됨)

## 제약 / TODO

- SSD1306 전용. 다른 컨트롤러(SH1106 등)로 바뀌면 `display_ui.c`의 init 커맨드 시퀀스부터 재검토 필요
- 메뉴 화면은 텍스트만 지원(도형/아이콘 없음), 항목 3개(COMM/IDLE/OTA) 고정 레이아웃 — 항목 개수가 바뀌면 `display_ui.c`의 `MENU_ITEM_*` 상수/`s_menu_labels` 배열을 손봐야 함
- 매 호출마다 8페이지 전체를 I2C로 flush해서(약 1KB) 레거시 1-row 갱신보다 느림 — 메뉴 UI라 빈도가 낮아 지금은 문제없지만, 고빈도 갱신이 필요해지면 변경분만 flush하는 최적화 고려
