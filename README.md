# firmware-esp32

ESP32-S3 기반 무전기 단말 펌웨어 — FHSS 음성 통신 + RF OTA 수신 담당.

## 원본 저장소

- 조직: https://github.com/fhss-ota-radio
- 이 레포(develop 브랜치, 최신 상태): https://github.com/fhss-ota-radio/firmware-esp32/tree/develop
- 기술적인 내용·설계 배경·상세 이슈는 위 develop 브랜치의 커밋 기록과 `docs/`를 참고할 것. 이 문서는 빌드 방법 안내만 다룸.
- 기술적인 문의: 설동호(theseol16@gmail.com)

## 제작자

- 설동호 — https://github.com/hiimseoll
- 조민진 — https://github.com/CHOminjin
- 김지윤 — https://github.com/JJiiyun

## 빌드 환경

- 대상 칩: ESP32-S3
- 보드: N16R8(Flash 16MB) 기본. N8R8(Flash 8MB) 사용 시 별도 오버레이 필요(아래 참고)
- ESP-IDF: **v6.0.2** 고정 — 다른 버전에서는 빌드 실패/동작 불일치 가능성 있음. 설치 방법은 [Espressif 공식 문서](https://docs.espressif.com/projects/esp-idf/en/v6.0.2/esp32s3/get-started/) 참고

## 필요 부품

| 부품 | 사양 | 비고 |
|---|---|---|
| ESP32-S3 개발보드 | N16R8(Flash 16MB) 권장 | 온보드 WS2812 RGB LED 포함 |
| RF 모듈 | CC1101, 433MHz | 안테나 필수 |
| 마이크 모듈 | INMP441 (I2S) | |
| 스피커 앰프 모듈 | MAX98357A (I2S) + 스피커 | |
| OLED 디스플레이 | SSD1306, 128x64, I2C (주소 0x3C) | |
| 푸시 버튼 | 모멘터리 스위치 1개 | PTT용 |
| 로터리 엔코더 | 푸시 스위치 내장형 | 메뉴 조작용 |

## 핀맵

| 부품 | 신호 | GPIO |
|---|---|---|
| CC1101 | SCLK | 12 |
| CC1101 | MOSI | 9 |
| CC1101 | MISO | 11 |
| CC1101 | CS | 10 |
| CC1101 | GDO0 (인터럽트) | 13 |
| 마이크 (INMP441) | WS / LRCLK | 4 |
| 마이크 (INMP441) | BCLK / SCK | 5 |
| 마이크 (INMP441) | SD (데이터) | 6 |
| 스피커 (MAX98357A) | BCLK / SCK | 15 |
| 스피커 (MAX98357A) | WS / LRC | 7 |
| 스피커 (MAX98357A) | DIN | 16 |
| 스피커 (MAX98357A) | GAIN | 17 |
| 스피커 (MAX98357A) | SD (셧다운) | 18 |
| OLED | SDA | 21 |
| OLED | SCL | 20 |
| PTT 버튼 | 입력 | 1 |
| 로터리 엔코더 | A | 2 |
| 로터리 엔코더 | B | 42 |
| 로터리 엔코더 | SW | 41 |
| 상태 LED (WS2812) | 데이터 | 38 |

⚠️ **GPIO14는 보드 배선상 GND와 직결 — 어떤 용도로도 사용 금지** (HIGH 출력 시 쇼트)

## 빌드 전 필수 준비

### 1. 서브모듈 초기화

git clone만으로는 아래 두 서브모듈이 비어있음 — 빌드 전 반드시 실행:

```bash
git submodule update --init --recursive
```

(clone 시점에 `--recurse-submodules`를 같이 줬다면 생략 가능)

- `components/audio_codec/speex` — xiph/speex 원본
- `components/ota_protocol/upstream` — 게이트웨이와 공유하는 OTA 와이어 프로토콜

### 2. `CONFIG_FREERTOS_HZ` 확인

- 기본값 `100`이면 `rotary_encoder`/`ptt_button` 폴링이 CPU 99% busy-loop 버그로 재현됨 — 반드시 `1000`이어야 함
- 새로 clone: `sdkconfig.defaults`에 이미 `1000`으로 들어있어 조치 불필요
- 기존 `sdkconfig`가 남아있는 로컬 환경: `idf.py menuconfig` > Component config > FreeRTOS > Kernel > Tick rate에서 직접 확인

### 3. `secret_seed.txt` 준비 — 없으면 빌드 실패

`components/fhss_audio_adapter/secret_seed.txt`는 FHSS hop_seed를 HMAC-SHA256으로 파생할 때 쓰는 팀 공유 비밀. `EMBED_TXTFILES`로 펌웨어에 직접 박아 넣는 파일이라 git에는 없음(gitignore 대상).

- `components/fhss_audio_adapter/secret_seed.txt.example`을 참고해 같은 위치에 `secret_seed.txt`를 직접 생성
- 형식: 8자리 16진수 한 줄(예: `4B434349`)
- **통신할 모든 기기가 완전히 동일한 값을 가져야 함** — 하나라도 다르면 HMAC 결과가 완전히 달라져 홉이 안 맞아 통신 자체가 안 됨(랑데부 채널에서 잠깐 붙었다 바로 끊기는 증상으로 나타남)

### 4. N8R8 보드 사용 시(선택)

팀 기본값은 N16R8이라 옵션 없는 `idf.py build`는 그대로 N16R8로 빌드됨. 부품 수급 문제로 N8R8을 쓰는 경우만 아래가 필요:

```bash
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.n8r8" build
```

N16R8 ↔ N8R8을 오가며 빌드할 때는 `sdkconfig`를 반드시 먼저 삭제할 것(`idf.py fullclean`은 `build/`만 지우고 `sdkconfig`는 안 지움 — 이전 보드의 Flash 크기 설정이 그대로 남아 부팅 실패로 이어짐):

```bash
rm -f sdkconfig && idf.py fullclean
```

## 빌드 및 플래시

```bash
idf.py set-target esp32s3
idf.py build
idf.py -p PORT flash monitor
```

## 핵심 기능

- I2S 마이크/스피커 입출력, PTT 제어
- Speex Narrowband(8kHz) 음성 압축/해제
- OLED 상태 표시(채널/배터리/TX-RX/OTA 진행률)
- CC1101 단일 라디오로 음성 FHSS 호핑 동기화 + OTA 청크 수신/파티션 전환·롤백 겸용

## 컴포넌트 구조

```
firmware-esp32/
├── main/                  # 통합 진입점(컴포넌트 wiring, 전역 FSM)
├── components/
│   ├── audio_io/          # I2S 마이크/스피커
│   ├── audio_codec/       # Speex 인코더/디코더 (submodule)
│   ├── display_ui/        # OLED 상태 표시
│   ├── ptt_button/        # PTT 디바운스
│   ├── rotary_encoder/    # 메뉴 커서
│   ├── status_led/        # 온보드 RGB LED 상태 표시
│   ├── device_id/         # 기기 고유 식별자(eFuse MAC 뒤 3바이트)
│   ├── ota_client/        # OTA 세션/청크 검증, 파티션 기록
│   ├── ota_protocol/      # 공유 와이어 포맷 (submodule)
│   ├── ota_rf_bridge/     # RF 수신 패킷 → ota_client 큐 어댑터
│   ├── fhss_core/         # 호핑 시퀀스, 동기화 로직
│   ├── fhss_service/      # fhss_core+rf_transport 상위 서비스
│   ├── fhss_audio_adapter/# FHSS ↔ 오디오/OTA 연동, secret_seed 기반 hop_seed 파생
│   └── rf_transport/      # CC1101 SPI 드라이버
└── docs/
```
