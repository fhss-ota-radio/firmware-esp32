# display_ui

0.96" I2C OLED(SSD1306, 128x64) 상태 표시 컴포넌트.

## 구성

| 파일 | 내용 |
|---|---|
| `display_ui_config.h` | 핀/주소/해상도 설정. **하드웨어 바뀌면 여기만 수정** |
| `display_ui.h` | 공개 API |
| `display_ui.c` | SSD1306 드라이버 구현 (`driver/i2c_master.h` 기반) |
| `font8x8_basic.h` | 8x8 비트맵 폰트, public domain ([dhepper/font8x8](https://github.com/dhepper/font8x8)) |

## 사용법

```c
#include "display_ui.h"

display_ui_init();                          // 부팅 시 1회
oled_update_text(0, "CH 3  BAT 82%");        // row 0에 텍스트 표시
oled_update_text_fmt(1, "TX %d dBm", -42);   // printf 스타일
display_ui_clear();                          // 전체 지우기
```

- `row` 범위: `0 ~ DISPLAY_UI_TEXT_ROWS-1` (128x64 기준 0~7)
- 한 줄 최대 `DISPLAY_UI_TEXT_COLS`(16)자, 초과분은 잘림
- `oled_update_text*()` 호출 시 해당 행만 갱신 (전체 화면 재전송 안 함)

## 하드웨어 배선 확정 시 수정할 것 (`display_ui_config.h`)

- `DISPLAY_UI_I2C_SDA_GPIO` / `DISPLAY_UI_I2C_SCL_GPIO` — 현재 SDA=GPIO21, SCL=GPIO47(실기기 테스트 배선). 최종 배선 확정 전까지는 바뀔 수 있음, 다른 보드/배선이면 이 값만 수정
- `DISPLAY_UI_I2C_ADDR` — 기본 `0x3C`. 모듈에 따라 `0x3D`인 경우도 있음, 안 켜지면 제일 먼저 의심
- `DISPLAY_UI_I2C_PORT` — 다른 I2C 장치와 버스 공유 시 포트 번호 조정
- `DISPLAY_UI_WIDTH` / `HEIGHT` — 128x64 아닌 다른 크기 모듈로 바뀌면 수정 (128x32 등)

## 제약 / TODO

- SSD1306 전용. 다른 컨트롤러(SH1106 등)로 바뀌면 `display_ui.c`의 init 커맨드 시퀀스부터 재검토 필요
- 텍스트 표시만 지원 (도형/아이콘 렌더링 없음)
- `main.c`에 아직 연결 안 됨 — 컴포넌트 단독 빌드만 가능한 상태
