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
│   ├── rotary_encoder/    # 팀원1 — 메뉴 커서, EV_MENU_SELECT_IDLE/OTA
│   └── status_led/        # 팀원1 — 온보드 RGB LED 상태 표시 (디버그용)
└── docs/
```

## 현재 구현 현황 (`test/speaker-amp-wiring`)

**2026-08-10: 실기기 첫 검증 성공** — OLED/PTT/LED/마이크 캡처가 실제 ESP32-S3 보드에서 정상 동작 확인됨 (PTT 누르면 FSM이 `TX_AUDIO`로 실제 전이, LED 점등, 크래시 없음). 앰프(MAX98357A) GAIN/SD GPIO 제어 + PTT 삐빅음 테스트 진행 중. 아래 각 항목에 반영.
- [x] `components/audio_codec/` — Speex 코덱 컴포넌트
  - `speex/` — xiph/speex 원본 (git submodule, pristine 유지)
  - `CMakeLists.txt` — ESP-IDF 빌드용 래퍼 (협대역 전용 소스만 선별)
  - `audio_codec.h` / `audio_codec.c` — encode/decode 얇은 래퍼
- [x] `main/fsm.c` / `main/fsm.h` — 단말 전체 통합 FSM (설계는 [docs/fsm-design.md](fhss-ota-radio/docs/fsm-design.md) 참고), `main.c`에서 `fsm_init()`으로 wiring
  - **메뉴 기반 수신 모드 게이팅 추가**: 기존 단일 `IDLE` 상태를 `MENU_IDLE`(음성, 기본)/`MENU_OTA`(OTA 대기)로 분리. 로터리 엔코더 클릭 이벤트(`EV_MENU_SELECT_IDLE`/`EV_MENU_SELECT_OTA`)로만 전환되며, 음성 송수신·OTA 수신/적용 중에는 이 전이가 정의돼 있지 않아 메뉴 변경이 SW적으로 불가능함
  - 부수 효과: 음성 통화 중 OTA가 강제로 끼어드는 이전 전이(`TX_AUDIO`/`RX_AUDIO` → `OTA_RECEIVING` on `EV_OTA_START`)는 폐기 — 이제 `MENU_OTA`에서만 OTA 수신이 유효함
- [x] `components/display_ui/` — 0.96" I2C OLED(SSD1306, 128x64) 상태 표시 컴포넌트, **실기기 테스트 완료**
  - `display_ui_config.h` — I2C 핀 SDA=GPIO21/SCL=GPIO20(2026-08-11 브레드보드 재구성 배선, 최종 확정 전까지 바뀔 수 있음)·주소·해상도 매크로
  - `display_ui.h` / `display_ui.c` — `driver/i2c_master.h`(ESP-IDF v5.2+ 신규 API) 기반 SSD1306 드라이버
  - `font8x8_basic.h` — 공개도메인 8x8 비트맵 폰트 ([dhepper/font8x8](https://github.com/dhepper/font8x8) 원본과 바이트 단위 대조 검증)
  - 공개 API: `display_ui_init()`, `display_ui_clear()`, `oled_update_text(row, text)`, `oled_update_text_fmt(row, fmt, ...)`
- [x] `components/ptt_button/` — PTT 버튼 디바운스 컴포넌트
  - `ptt_button_config.h` — 핀(GPIO1, 2026-08-11 브레드보드 재구성 배선)/active level/디바운스 파라미터
  - `ptt_button.h` / `ptt_button.c` — 폴링 기반 디바운스(ISR 미사용), 콜백/폴링 API 제공
  - 공개 API: `ptt_button_init()`, `ptt_button_set_callback(cb, ctx)`, `ptt_button_is_pressed()`
- [x] `components/rotary_encoder/` — 메뉴 선택용 로터리 엔코더(A/B/SW) 컴포넌트
  - `rotary_encoder_config.h` — 핀 A/B/SW=GPIO8/9/15(placeholder). **주의**: GPIO5/6/7은 audio_io 마이크와 겹쳐서 사용 금지(겹치면 I2S BCLK 토글이 회전으로 오인되는 버그 있었음, 2026-08-10). SW는 2026-08-11 audio_io 스피커 SD가 GPIO10을 쓰게 되면서 GPIO10→GPIO15로 이동
  - `rotary_encoder.h` / `rotary_encoder.c` — quadrature 폴링 디코딩 + SW 디바운스, 커서 이동은 순환(`MENU_IDLE ↔ MENU_OTA`)
  - 공개 API: `rotary_encoder_init()`, `rotary_encoder_set_cursor_callback()`, `rotary_encoder_set_select_callback()`, `rotary_encoder_get_cursor()`
- [x] `components/audio_io/` — I2S 마이크(INMP441)/스피커(MAX98357A) 입출력 + audio_codec 연결, **마이크 캡처 실기기 테스트 완료, 앰프 배선/테스트 진행 중**
  - `audio_io_config.h` — 마이크(GPIO5/6/7)/스피커(LRC=14,BCLK=13,DIN=12,GAIN=11,SD=10, 2026-08-11 브레드보드 재구성 배선) 핀 설정
  - 마이크(RX)=`I2S_NUM_0`, 스피커(TX)=`I2S_NUM_1` 별도 포트 고정 배정 (재설정 없이 동시 존재)
  - 공개 API: `audio_io_init()`, `audio_io_capture_encode(out, cap)`, `audio_io_decode_play(data, len)`, `audio_io_speaker_enable()`/`audio_io_speaker_disable()`, `audio_io_play_beep()` — 내부에서 `audio_codec_encode/decode` 호출
  - **스피커 채널은 지연 활성화**: 마이크와 달리 `audio_io_init()`에서 채널만 만들고 enable 안 함 — 실제 재생 시점(`RX_AUDIO` 진입, 또는 삐빅음 재생 직전)에만 `audio_io_speaker_enable()`로 켬. 켜둔 채 한 번도 안 쓰면 GDMA TX 인터럽트가 NULL 컨텍스트로 불려 재부팅되는 문제가 실기기에서 확인돼 수정함(2026-08-10)
  - **GAIN/SD GPIO 제어 추가**: GAIN은 항상 HIGH(VDD)=6dB 고정(GPIO로 저항 없이 가능한 값 중 최소), SD는 `audio_io_speaker_enable/disable()`에서 HIGH(왼쪽 채널 출력)/LOW(완전 꺼짐)로 제어
  - **PTT 테스트용 삐빅음**: `RX_AUDIO`(수신 재생) 미구현 상태에서 앰프 동작 확인용으로 `audio_io_play_beep()` 추가 — A5(880Hz)→D6(1175Hz) 2음. 볼륨은 GAIN 6dB + 소프트웨어 진폭(`AUDIO_IO_BEEP_AMPLITUDE`=500/32767, 약 1.5%)로 이중으로 최소화
- [x] **`display_ui`/`ptt_button`/`rotary_encoder`/`audio_io` → FSM wiring 완료** (`main/fsm.c`)
  - 부팅 시(`on_enter_boot_init()`) 네 컴포넌트 `init()` + 콜백 등록: `ptt_button` press/release → `FSM_EVENT_PTT_PRESS/RELEASE`, `rotary_encoder` 클릭 → `FSM_EVENT_MENU_SELECT_IDLE/OTA`, 로터리 회전 → `oled_update_text()`로 미리보기만 갱신(FSM 이벤트 아님), `audio_codec_init()`/`audio_io_init()`
  - `on_enter_menu_idle`/`on_enter_menu_ota`에서 OLED에 현재 모드 표시
  - `on_enter_tx_audio()`: **`fsm_task` 안에서 블로킹으로** 스피커 켜고 `audio_io_play_beep()` 삐빅음 재생 후 다시 끔(앰프 테스트용) → 그 다음에야 `tx_audio_task`(마이크 20ms 캡처 루프, 스택 8192) 생성. 삐빅음을 태스크 안이 아니라 `fsm_task`에서 블로킹으로 끝까지 재생하는 이유: PTT를 삐빅음 재생 중(~210ms 이내)에 떼면 `vTaskDelete()`가 스피커 disable 전에 태스크를 강제 종료시켜 삐빅음이 안 끝나고 계속 재생되는 버그가 있었음 — `fsm_task`는 이벤트를 하나씩 순서대로 처리하니 삐빅음이 끝나야 `PTT_RELEASE`를 처리해서 이 경쟁 상태를 없앰(진단 과정은 로컬 `troubleshoot/요약.md` 참고, git 비관리). PTT_RELEASE로 `MENU_IDLE` 재진입 시(`on_enter_menu_idle()`) `tx_audio_task` 정리. **실기기 검증 완료** — PTT 눌러서 `MENU_IDLE → TX_AUDIO` 실제 전이 확인
  - `on_enter_rx_audio()`: `audio_io_speaker_enable()`로 스피커 켠 뒤, 오디오 프레임 전용 큐(`fsm_post_rx_audio_frame(data, len)`, `main/fsm.h` 신규 API)에서 프레임을 꺼내 `audio_io_decode_play()`로 재생하는 태스크(스택 8192) 시작, `on_enter_menu_idle()`에서 태스크 정리 + `audio_io_speaker_disable()` — `fsm_event_t`(페이로드 없는 enum)와 별개 큐로 데이터 전달
  - `FSM_EVENT_RX_DONE`은 무음 타임아웃 1초(`FSM_RX_AUDIO_IDLE_TIMEOUT_MS`)로 확정 — `rx_audio_task`가 1초간 새 프레임 없으면 스스로 이벤트를 올리고 종료 (`rf_transport` 없어 아직 실측 검증은 안 됨)
  - **미정으로 명시해둔 것**: `fsm_post_rx_audio_frame()`을 실제 호출할 `rf_transport`가 아직 없어 큐가 항상 비어있음 (`docs/fsm-design.md` 결정 이력 2026-08-06 참고)
  - **의도적으로 안 채운 것**: `ota_*`는 그대로 TODO — `rf_transport`가 없어 실제 수신 트리거가 없음
  - **`FHSS_SYNC` 상태/`SYNC_ACQUIRED` 이벤트 제거(2026-08-10)**: 브로드캐스트 설계로 확정(PTT 누른 쪽이 정해진 시작 채널로 먼저 송신 후 시드 기반 호핑, 받는 쪽은 그 수신 시점 기준으로 추종) — 별도 "동기 획득 대기" 상태가 불필요해져 `FSM_STATE_FHSS_SYNC`/`FSM_EVENT_SYNC_ACQUIRED`를 `fsm.h`/`fsm.c`에서 삭제하고 `BOOT_INIT`이 곧바로 `MENU_IDLE`로 전이하도록 변경. **이제 PTT를 누르면 FSM이 실제로 `TX_AUDIO`로 전이한다** (예전엔 `FHSS_SYNC`에 멈춰 unhandled로 무시됐음)
  - `FSM_EVENT_SYNC_LOST`는 전역 안전장치 이벤트로 유지 — 목적지만 `FHSS_SYNC`에서 `MENU_IDLE`로 변경. 무선 계층(`rf_transport`/`fhss_core`)이 홉 추종 실패를 판단하면 이 이벤트로 강제 복귀시키는 용도(팀5의 `fhss_sync_state` 모듈이 판정 로직 후보, 아직 미완성)
  - **알려진 제약**: `rf_transport`가 없어 `TX_AUDIO`에서 캡처한 프레임을 실제로 보낼 곳도, `RX_AUDIO`가 받을 실제 프레임도 없음 — 그래서 이 wiring은 컴파일/개별 컴포넌트 단위 검증까지만 가능하고, `rf_transport` 생기기 전까지 실기기 end-to-end 테스트는 불가
- [x] `components/ota_client/` — OTA 세션/청크 검증/플래시 기록 컴포넌트 (팀2, 별도 브랜치에서 병합됨) — `rf_transport`(무선 송수신)가 아직 없어 실제 동작은 불가, 역할 분리만 잡혀있는 상태 (자세한 내용은 [components/ota_client/README.md](fhss-ota-radio/components/ota_client/README.md))
- [x] `components/status_led/` — 온보드 WS2812 RGB LED(GPIO38, `led_strip` managed component) 상태 표시 (디버그용)
  - `main/fsm.c`의 `on_ptt_event()`에 직접 연결 — FSM 처리 결과를 기다리지 않고 GPIO 디바운스만 통과하면 바로 켜짐/꺼짐 (FSM 전이표 변경과 무관하게 동작)
- [x] `components/fhss_core/` — `fhss_sync_packet.c/h`(동기 패킷 encode/decode, big-endian 13바이트 와이어 포맷) 구현됨 (팀5, 별도 브랜치에서 병합됨)
  - `fhss_hop_sequence.c/h`는 아직 빈 스텁 — 호핑 시퀀스 계산 로직 미구현
- [ ] CC1101 저수준 SPI(`rf_transport`), `fhss_hop_sequence` 실구현, `fhss_sync_state`(SEARCHING/LOCKED 판정, 헤더 비어있어 빌드 안 됨)는 아직 미완성 — 이게 없어서 FSM wiring도, `ota_client`도 실기기에서는 검증 못 하는 상태

## 담당
팀원1 (오디오/OLED), 팀원2 (OTA/부트로더), 팀원5 (FHSS)
