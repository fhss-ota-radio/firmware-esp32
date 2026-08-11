# rotary_encoder

로터리 엔코더(A/B + 클릭 스위치) 드라이버. 메뉴 커서 이동/확정 담당. `main/fsm.c`에 연결됨.

## 구성

| 파일 | 내용 |
|---|---|
| `rotary_encoder_config.h` | 핀(A/B/SW)/디바운스/detent 파라미터. **하드웨어 바뀌면 여기만 수정** |
| `rotary_encoder.h` | 공개 API |
| `rotary_encoder.c` | quadrature 디코딩 + SW 디바운스 구현 |

## 동작 방식

- ISR 안 씀. `ROTARY_ENCODER_POLL_MS`(기본 2ms)마다 A/B/SW 폴링하는 단일 태스크
- 회전: A/B 두 비트를 그레이코드 순번으로 변환해 1-step씩 누적, **정지 위치(A/B 둘 다 released)로 돌아왔을 때만** detent 확정 → 접점 튐/중간 정지에 안전
- 클릭(SW): ptt_button과 동일한 "연속 N회 안정" 폴링 디바운스, **눌리는 순간에만** select 콜백 발생 (뗄 때는 발생 안 함)
- 메뉴 순회: `docs/fsm-design.md` 기준 `COMM(0) -> IDLE(1) -> OTA(2) -> COMM`, 시계방향 회전 = 아래로 이동(화면에 위에서부터 COMM/IDLE/OTA 순서로 표시됨과 일치)

## 사용법

```c
#include "rotary_encoder.h"

static void on_cursor_move(rotary_encoder_menu_t cursor, void *ctx)
{
    // main/fsm.c: display_ui_draw_menu()로 흰 테두리(hover) 갱신
}

static void on_select(rotary_encoder_menu_t selected, void *ctx)
{
    // main/fsm.c: selected에 따라
    //   ROTARY_ENCODER_MENU_COMM -> fsm_post_event(FSM_EVENT_MENU_SELECT_COMM)
    //   ROTARY_ENCODER_MENU_IDLE -> fsm_post_event(FSM_EVENT_MENU_SELECT_IDLE)
    //   ROTARY_ENCODER_MENU_OTA  -> fsm_post_event(FSM_EVENT_MENU_SELECT_OTA)
}

rotary_encoder_init();
rotary_encoder_set_cursor_callback(on_cursor_move, NULL);
rotary_encoder_set_select_callback(on_select, NULL);

// 콜백 대신 폴링도 가능
rotary_encoder_menu_t cur = rotary_encoder_get_cursor();
```

## 하드웨어 배선 확정 시 수정할 것 (`rotary_encoder_config.h`)

- `ROTARY_ENCODER_GPIO_A` / `_B` / `_SW` (모듈 라벨 S1/S2/KEY) — 현재 GPIO2/42/41은 **placeholder** (2026-08-11 재배정, GPIO1은 ptt_button과 겹쳐서 사용 금지)
- 방향이 반대로 돈다고 느껴지면 **A/B 핀 배선(또는 config의 A/B 매크로)을 서로 바꾸기** — 코드 로직 안 건드려도 됨
- `ROTARY_ENCODER_STEPS_PER_DETENT` — 딸깍 1칸에 메뉴가 안 움직이거나 여러 칸씩 튀면 조정 (기본 4)
- `ROTARY_ENCODER_SW_ACTIVE_LOW` — SW를 GND로 물리면 1(내부 풀업), 3.3V로 물리면 0(내부 풀다운)

## 제약 / TODO

- 정지 위치가 "A/B 둘 다 released(HIGH)"인 모듈 기준으로 구현 (`rotary_encoder.c`의 `REST_AB`). 반대(풀다운) 모듈이면 `REST_AB`와 `gpio_config()`의 pull 설정을 같이 바꿔야 함
- 2ms 폴링이라 매우 빠르게 돌리면 일부 step을 놓칠 수 있음 (필요시 ISR 방식으로 교체 고려)
