# device_id

기기 고유 식별자 컴포넌트. ESP32-S3 eFuse에 공장에서 구워진 base MAC
주소 뒤 `DEVICE_ID_LEN`(3)바이트를 그대로 기기 고유번호로 쓴다.

## 구성

| 파일 | 내용 |
|---|---|
| `device_id_config.h` | `DEVICE_ID_LEN`(=3) 정의 및 근거 |
| `device_id.h` | 공개 API |
| `device_id.c` | `esp_efuse_mac_get_default()` 래퍼 |

## 설계

- MAC 주소 앞 3바이트(OUI)는 같은 Espressif 칩을 쓰는 모든 보드가
  동일해서 구분에 의미가 없음 — 실질적 고유성은 뒤 3바이트(칩마다
  공장에서 고유 배정되는 NIC 부분)에 있음
- eFuse는 OTP(One-Time Programmable)라 펌웨어를 몇 번을 재플래시해도
  값이 안 바뀜 — 기기별로 빌드를 따로 만들거나 패치마다 값을 수정할
  필요 없이, 모든 기기에 같은 펌웨어를 올려도 각자 알아서 자기 고유값을
  가짐
- Wi-Fi/BT 스택 초기화 여부와 무관하게 eFuse만 읽으므로 `audio_io_init()`
  등과 순서 상관없이 부팅 후 아무 때나 호출 가능

## 사용법

```c
#include "device_id.h"

uint8_t id[DEVICE_ID_LEN];
device_id_get(id);              // 예: {0x4A, 0x1B, 0xC7}

char hex[DEVICE_ID_LEN * 2 + 1];
device_id_get_hex(hex, sizeof(hex));  // 예: "4A1BC7" (로그/OLED 표시용)
```

## 용도(예정)

- Qt 앱이 OTA 스캔 신호를 보내면, `MENU_OTA` 상태의 ESP가 ACK 응답
  페이로드에 `device_id_get()`의 3바이트를 실어 회신 — 여러 기기가 동시에
  OTA 대기 중이어도 Qt 앱이 응답자를 구분할 수 있게 함
- 실제 ACK 패킷 조립/응답 로직은 `rf_transport`/`ota_client`가 아직
  없어 미구현 — 이 컴포넌트는 그 인프라만 먼저 준비해둔 상태
