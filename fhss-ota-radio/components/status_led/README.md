# status_led

온보드 WS2812 RGB LED(GPIO38, RMT 기반) 상태 표시 컴포넌트. 디버그/테스트용.

## 구성

| 파일 | 내용 |
|---|---|
| `status_led_config.h` | GPIO 핀·LED 개수·밝기 설정. **보드 바뀌면 여기만 수정** |
| `status_led.h` | 공개 API |
| `status_led.c` | `led_strip`(Espressif managed component) 래퍼 |
| `idf_component.yml` | `led_strip` 의존성 선언 (빌드 시 컴포넌트 매니저가 자동으로 받아옴) |

## 사용법

```c
#include "status_led.h"

status_led_init();          // 부팅 시 1회
status_led_set_white_dim(); // 흰색 약하게 켜기
status_led_off();           // 끄기

status_led_start_error_blink(); // 빨간 점멸 시작(내부 esp_timer, PTT 흰색과 구분)
status_led_stop_error_blink();  // 점멸 멈추고 끄기
```

## 하드웨어 바뀌면 수정할 것 (`status_led_config.h`)

- `STATUS_LED_GPIO` — 현재 38(devkit 실물 기준). 일부 리비전은 48을 쓰므로 보드 다르면 확인 필요
- `STATUS_LED_DIM_BRIGHTNESS` — 너무 밝으면 낮추기 (0~255)
- `STATUS_LED_BLINK_INTERVAL_MS` — ERROR 점멸 주기(on/off 각각), 기본 300ms

## 연결 현황

- `main/fsm.c`의 `on_ptt_event()` → 실제로 수음(마이크 캡처)으로 이어지는 PTT일 때만 흰색 켬(2026-08-16 수정, 이전엔 상태 무관하게 원시 입력을 그대로 반영해 MENU_OTA 등 캡처 없는 상태에서도 켜지는 문제가 있었음): `MENU_COMM`에서 누르면 켜짐(전이표상 `TX_AUDIO`로 이어짐), `LOOPBACK_ENABLE` 켜져있으면 `MENU_IDLE`의 마이크 loopback 녹음 중에도 켜짐. 뗄 때는 상태 무관하게 항상 꺼서 `EV_ERROR`/`EV_SYNC_LOST` 등으로 도중에 상태가 바뀌어도 켜진 채로 안 남게 함
- `main/fsm.c`의 `on_enter_error()` → `EV_ERROR`로 `ERROR` 상태 진입 시 빨간 점멸 시작, `EV_RETRY`로 빠져나갈 때(`on_enter_boot_init()`) 정지

## 제약 / TODO

- LED 1개(단일 픽셀)만 지원
