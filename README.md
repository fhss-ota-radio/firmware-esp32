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
│   ├── rf_transport/      # 팀원5(+2) — nRF24/CC1101 저수준 SPI
│   ├── ptt_button/        # 팀원1 — PTT 디바운스, EV_PTT_PRESS/RELEASE
│   └── rotary_encoder/    # 팀원1 — 메뉴 커서, EV_MENU_SELECT_IDLE/OTA
└── docs/
```

## 현재 구현 현황 (`feature/audio_io` → `develop` 병합 후)
- [x] `components/audio_codec/` — Speex 코덱 컴포넌트
  - `speex/` — xiph/speex 원본 (git submodule, pristine 유지)
  - `CMakeLists.txt` — ESP-IDF 빌드용 래퍼 (협대역 전용 소스만 선별)
  - `audio_codec.h` / `audio_codec.c` — encode/decode 얇은 래퍼
- [x] `main/fsm.c` / `main/fsm.h` — 단말 전체 통합 FSM (설계는 [docs/fsm-design.md](fhss-ota-radio/docs/fsm-design.md) 참고), `main.c`에서 `fsm_init()`으로 wiring
  - **메뉴 기반 수신 모드 게이팅 추가**: 기존 단일 `IDLE` 상태를 `MENU_IDLE`(음성, 기본)/`MENU_OTA`(OTA 대기)로 분리. 로터리 엔코더 클릭 이벤트(`EV_MENU_SELECT_IDLE`/`EV_MENU_SELECT_OTA`)로만 전환되며, 음성 송수신·OTA 수신/적용 중에는 이 전이가 정의돼 있지 않아 메뉴 변경이 SW적으로 불가능함
  - 부수 효과: 음성 통화 중 OTA가 강제로 끼어드는 이전 전이(`TX_AUDIO`/`RX_AUDIO` → `OTA_RECEIVING` on `EV_OTA_START`)는 폐기 — 이제 `MENU_OTA`에서만 OTA 수신이 유효함
- [x] `components/display_ui/` — 0.96" I2C OLED(SSD1306, 128x64) 상태 표시 컴포넌트
  - `display_ui_config.h` — I2C 핀(SDA/SCL, placeholder)·주소·해상도 매크로 (배선 확정 시 이 파일만 수정)
  - `display_ui.h` / `display_ui.c` — `driver/i2c_master.h`(ESP-IDF v5.2+ 신규 API) 기반 SSD1306 드라이버
  - `font8x8_basic.h` — 공개도메인 8x8 비트맵 폰트 ([dhepper/font8x8](https://github.com/dhepper/font8x8) 원본과 바이트 단위 대조 검증)
  - 공개 API: `display_ui_init()`, `display_ui_clear()`, `oled_update_text(row, text)`, `oled_update_text_fmt(row, fmt, ...)`
- [x] `components/ptt_button/` — PTT 버튼 디바운스 컴포넌트
  - `ptt_button_config.h` — 핀(placeholder)/active level/디바운스 파라미터
  - `ptt_button.h` / `ptt_button.c` — 폴링 기반 디바운스(ISR 미사용), 콜백/폴링 API 제공
  - 공개 API: `ptt_button_init()`, `ptt_button_set_callback(cb, ctx)`, `ptt_button_is_pressed()`
- [x] `components/rotary_encoder/` — 메뉴 선택용 로터리 엔코더(A/B/SW) 컴포넌트
  - `rotary_encoder_config.h` — 핀(A/B/SW, placeholder)/디바운스/detent 파라미터
  - `rotary_encoder.h` / `rotary_encoder.c` — quadrature 폴링 디코딩 + SW 디바운스, 커서 이동은 순환(`MENU_IDLE ↔ MENU_OTA`)
  - 공개 API: `rotary_encoder_init()`, `rotary_encoder_set_cursor_callback()`, `rotary_encoder_set_select_callback()`, `rotary_encoder_get_cursor()`
- [x] `components/audio_io/` — I2S 마이크(INMP441)/스피커(MAX98357A) 입출력 + audio_codec 연결
  - `audio_io_config.h` — 마이크/스피커 I2S 포트·핀(placeholder) 설정
  - 마이크(RX)=`I2S_NUM_0`, 스피커(TX)=`I2S_NUM_1` 별도 포트 고정 배정 (재설정 없이 동시 존재)
  - 공개 API: `audio_io_init()`, `audio_io_capture_encode(out, cap)`, `audio_io_decode_play(data, len)` — 내부에서 `audio_codec_encode/decode` 호출
- [x] **`display_ui`/`ptt_button`/`rotary_encoder` → FSM wiring 완료** (`main/fsm.c`의 `on_enter_boot_init()`)
  - 부팅 시 세 컴포넌트 `init()` + 콜백 등록: `ptt_button` press/release → `FSM_EVENT_PTT_PRESS/RELEASE`, `rotary_encoder` 클릭 → `FSM_EVENT_MENU_SELECT_IDLE/OTA`, 로터리 회전 → `oled_update_text()`로 미리보기만 갱신(FSM 이벤트 아님)
  - `on_enter_menu_idle`/`on_enter_menu_ota`에서 OLED에 현재 모드 표시
  - **의도적으로 안 채운 것**: `audio_io`/`rf_transport`가 필요한 `on_enter_tx_audio`/`rx_audio`/`fhss_sync`/`ota_*`는 그대로 TODO — `audio_io`가 이제 존재하지만 아직 이 wiring에는 반영 안 됨 (다음 작업 대상)
  - **알려진 제약**: `rf_transport`/`fhss_core`가 없어 `FSM_EVENT_SYNC_ACQUIRED`를 아무도 안 올림 → 부팅 후 `FHSS_SYNC`에서 멈추고 `MENU_IDLE`(PTT/로터리가 실제로 쓰이는 상태)에 도달 못 함. 임시 bypass는 의도적으로 넣지 않음(이유는 `docs/fsm-design.md` 결정 이력 2026-08-05 참고) — 그래서 이 wiring은 컴파일/개별 컴포넌트 단위 검증까지만 가능하고, `rf_transport` 생기기 전까지 실기기 end-to-end 테스트는 불가
- [x] `components/ota_client/` — OTA 세션/청크 검증/플래시 기록 컴포넌트 (팀2, 별도 브랜치에서 병합됨) — `rf_transport`(무선 송수신)가 아직 없어 실제 동작은 불가, 역할 분리만 잡혀있는 상태 (자세한 내용은 [components/ota_client/README.md](fhss-ota-radio/components/ota_client/README.md))
- [ ] `audio_io`는 컴포넌트만 존재, FSM(TX_AUDIO/RX_AUDIO)에 아직 미연결 — `audio_io_capture_encode`/`decode_play` 호출하는 배선은 TODO
- [ ] FHSS 호핑(`fhss_core`), CC1101 저수준 SPI(`rf_transport`)는 아직 미구현 — 이 둘이 없어서 위 wiring도, `ota_client`도 실기기에서는 검증 못 하는 상태

## 담당
팀원1 (오디오/OLED), 팀원2 (OTA/부트로더), 팀원5 (FHSS)
