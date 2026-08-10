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
```

## 하드웨어 바뀌면 수정할 것 (`status_led_config.h`)

- `STATUS_LED_GPIO` — 현재 38(devkit 실물 기준). 일부 리비전은 48을 쓰므로 보드 다르면 확인 필요
- `STATUS_LED_DIM_BRIGHTNESS` — 너무 밝으면 낮추기 (0~255)

## 제약 / TODO

- 현재 `main/fsm.c`의 PTT 눌림/뗌 표시용으로만 연결됨(하드웨어 동작 확인용 테스트 코드) — 다른 상태 표시(OTA 진행률 등)로 확장 가능
- LED 1개(단일 픽셀)만 지원
