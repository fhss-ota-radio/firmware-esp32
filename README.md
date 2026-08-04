# firmware-esp32

ESP32-S3 기반 무전기 단말 펌웨어. FHSS 음성 통신 + RF OTA 수신 담당.

## ⚠️ 빌드 전 준비 (필독)

`components/audio_codec/speex`는 git submodule(xiph/speex 원본)이라 **일반 clone만으로는 비어있습니다.** 빌드 전에 반드시 아래 중 하나를 실행하세요.

```bash
# 처음 clone할 때
git clone --recurse-submodules <repo-url>

# 이미 clone한 경우
git submodule update --init --recursive
```

이 폴더는 실제 소스가 아니라 "xiph/speex의 어느 커밋을 쓸지" 가리키는 포인터만 저장소에 커밋돼 있는 구조라, 각자 clone한 뒤 한 번씩 위 명령을 실행해야 합니다 (누가 먼저 실행했는지와 무관하게 매 clone마다 필요).

## 핵심 기능
- I2S 마이크/스피커 입출력, PTT 제어
- Speex Narrowband(8kHz) 음성 압축/해제
- OLED 상태 표시 (채널/배터리/TX-RX/OTA 진행률)
- CC1101 단일 라디오로 음성 FHSS 호핑 동기화 + OTA 청크 수신·파티션 전환/롤백 겸용 (nRF24L01 이원화는 보류)

## 컴포넌트 구조 (목표)
```
firmware-esp32/
├── main/                  # 통합 진입점 (컴포넌트 wiring, 전역 FSM)
├── components/
│   ├── audio_io/          # 팀원1 — I2S 마이크/스피커
│   ├── audio_codec/       # 팀원1 — Speex 인코더/디코더
│   ├── display_ui/        # 팀원1 — OLED 상태 표시
│   ├── ota_client/        # 팀원2 — OTA 수신, 파티션 전환, 롤백
│   ├── fhss_core/         # 팀원5 — 호핑 시퀀스, 동기화
│   └── rf_transport/      # 팀원5(+2) — nRF24/CC1101 저수준 SPI
└── docs/
```

## 현재 구현 현황 (`feature/oled_display_i2c_component`)
- [x] `components/audio_codec/` — Speex 코덱 컴포넌트
  - `speex/` — xiph/speex 원본 (git submodule, pristine 유지)
  - `CMakeLists.txt` — ESP-IDF 빌드용 래퍼 (협대역 전용 소스만 선별)
  - `audio_codec.h` / `audio_codec.c` — encode/decode 얇은 래퍼
- [x] `main/fsm.c` / `main/fsm.h` — 단말 전체 통합 FSM (설계는 [docs/fsm-design.md](fhss-ota-radio/docs/fsm-design.md) 참고), `main.c`에서 `fsm_init()`으로 wiring
- [x] `components/display_ui/` — 0.96" I2C OLED(SSD1306, 128x64) 상태 표시 컴포넌트
  - `display_ui_config.h` — I2C 핀(SDA/SCL, placeholder)·주소·해상도 매크로 (배선 확정 시 이 파일만 수정)
  - `display_ui.h` / `display_ui.c` — `driver/i2c_master.h`(ESP-IDF v5.2+ 신규 API) 기반 SSD1306 드라이버
  - `font8x8_basic.h` — 공개도메인 8x8 비트맵 폰트 ([dhepper/font8x8](https://github.com/dhepper/font8x8) 원본과 바이트 단위 대조 검증)
  - 공개 API: `display_ui_init()`, `display_ui_clear()`, `oled_update_text(row, text)`, `oled_update_text_fmt(row, fmt, ...)`
- [ ] audio_codec / display_ui 모두 아직 FSM/실제 앱 로직에 연결되지 않음 (컴포넌트만 빌드되는 상태, `main.c`는 손대지 않음)
- [ ] I2S 마이크/스피커, PTT, FHSS 호핑, CC1101 OTA 등 나머지 기능은 아직 미구현 — `components/{audio_io, ota_client, fhss_core, rf_transport}`는 목표 구조일 뿐 아직 생성 전

## 담당
팀원1 (오디오/OLED), 팀원2 (OTA/부트로더), 팀원5 (FHSS)
