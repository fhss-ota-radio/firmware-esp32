# ptt_button

PTT 버튼 GPIO 입력 + 디바운스 컴포넌트. FSM에는 아직 연결 안 됨.

## 구성

| 파일 | 내용 |
|---|---|
| `ptt_button_config.h` | 핀/active level/디바운스 파라미터. **하드웨어 바뀌면 여기만 수정** |
| `ptt_button.h` | 공개 API |
| `ptt_button.c` | 폴링 기반 디바운스 구현 |

## 동작 방식

- ISR 안 씀. 5ms(`PTT_BUTTON_POLL_MS`)마다 GPIO 폴링하는 전용 태스크로 디바운스
- 같은 레벨이 `PTT_BUTTON_DEBOUNCE_COUNT`(기본 6)회 연속 안정될 때만 상태 확정 → 기본 디바운스 30ms
- 상태 확정 시점에만 콜백 호출 (바운스 중에는 호출 안 됨)

## 사용법

```c
#include "ptt_button.h"

static void on_ptt(bool pressed, void *ctx)
{
    // TODO: FSM 연결 시 여기서 fsm_post_event(FSM_EVENT_PTT_PRESS/RELEASE) 호출
}

ptt_button_init();
ptt_button_set_callback(on_ptt, NULL);

// 콜백 대신 폴링도 가능
if (ptt_button_is_pressed()) { ... }
```

## 하드웨어 배선 확정 시 수정할 것 (`ptt_button_config.h`)

- `PTT_BUTTON_GPIO` — 현재 GPIO4는 **placeholder**, 실배선에 맞게 변경 필수
- `PTT_BUTTON_ACTIVE_LOW` — 버튼을 GND로 물리면 1(내부 풀업), 3.3V로 물리면 0(내부 풀다운)
- `PTT_BUTTON_POLL_MS` / `PTT_BUTTON_DEBOUNCE_COUNT` — 버튼 채터링이 심하면 디바운스 시간(둘의 곱) 늘리기

## 제약 / TODO

- FSM 미연결 상태 (`docs/fsm-design.md`의 `EV_PTT_PRESS`/`EV_PTT_RELEASE`) — 콜백에서 연결하면 됨
- `MENU_IDLE` 상태에서만 유효한 이벤트라는 규칙은 FSM 쪽에서 처리 (이 컴포넌트는 상태를 모름)
- 길게 누름(홀드)/짧게 누름 구분 없음, 단순 press/release만 보고
