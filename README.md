# firmware-esp32

ESP32-S3 기반 무전기 단말 펌웨어. FHSS 음성 통신 + RF OTA 수신 담당.

## 기기 고유 식별자 (`components/device_id/`)

ESP32-S3 eFuse에 공장에서 구워진 base MAC 뒤 3바이트(`DEVICE_ID_LEN`)를 그대로 사용 — OTP라 재플래시해도 안 바뀌고, 기기별 빌드 분리 불필요.

```c
uint8_t id[DEVICE_ID_LEN];
device_id_get(id);                    // 예: {0x4A, 0x1B, 0xC7}
device_id_get_hex(hex, sizeof(hex));  // 예: "4A1BC7"
```

용도(예정): OTA 스캔 시 `MENU_OTA` 기기가 ACK에 이 값을 실어 회신 → Qt 앱이 응답자 구분. 자세한 내용은 [components/device_id/README.md](fhss-ota-radio/components/device_id/README.md).

## ⚠️ 빌드 전 준비 (필독)

`components/audio_codec/speex`는 git submodule(xiph/speex 원본)이라 **일반 clone만으로는 비어있습니다.** 빌드 전에 반드시 아래 중 하나를 실행하세요.

```bash
# 처음 clone할 때
git clone --recurse-submodules <repo-url>

# 이미 clone한 경우
git submodule update --init --recursive
```

이 폴더는 실제 소스가 아니라 "xiph/speex의 어느 커밋을 쓸지" 가리키는 포인터만 저장소에 커밋돼 있는 구조라, 각자 clone한 뒤 한 번씩 위 명령을 실행해야 합니다 (누가 먼저 실행했는지와 무관하게 매 clone마다 필요).

**이미 로컬에 `sdkconfig`가 있다면 tick rate 확인**: `CONFIG_FREERTOS_HZ`가 `1000`인지 확인하세요(`sdkconfig.defaults`에 명시돼 있지만, 기존 `sdkconfig`가 있으면 자동 병합 안 됨). `100`(기본값)이면 `rotary_encoder`/`ptt_button`의 폴링이 `vTaskDelay(0)`이 되면서 사실상 busy-loop으로 CPU를 99% 먹는 버그가 재현됩니다(2026-08-12 확인, 로컬 `troubleshoot/task_wdt-poll_ms_zero_tick_busyloop.md` 참고) — `idf.py menuconfig` → Component config → FreeRTOS → Kernel → Tick rate에서 1000으로 바꾸세요. `sdkconfig`가 없는 새 clone은 `sdkconfig.defaults`가 자동 적용돼 신경 쓸 필요 없습니다.

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
│   ├── rotary_encoder/    # 팀원1 — 메뉴 커서, EV_MENU_SELECT_COMM/IDLE/OTA
│   ├── status_led/        # 팀원1 — 온보드 RGB LED 상태 표시 (디버그용)
│   └── device_id/         # 팀원1 — 기기 고유 식별자(MAC 뒤 3바이트)
└── docs/
```

## 현재 구현 현황 (`feature/device-id`)

**2026-08-10: 실기기 첫 검증 성공** — OLED/PTT/LED/마이크 캡처가 실제 ESP32-S3 보드에서 정상 동작 확인됨 (PTT 누르면 FSM이 `TX_AUDIO`로 실제 전이, LED 점등, 크래시 없음). 앰프(MAX98357A) GAIN/SD GPIO 제어 + PTT 삐빅음 테스트 진행 중.

**2026-08-11: OLED UI 재설계** — 화면을 좌측 90도 회전(세로)해서 COMM/IDLE/OTA 3항목 메뉴로 다시 그리고, FSM 메뉴도 2-way(IDLE/OTA)에서 **3-way(COMM/IDLE/OTA)**로 확장(기존 `MENU_IDLE`을 `MENU_COMM`으로 개명, 새 `MENU_IDLE`은 뮤트 상태). 아래 각 항목에 반영.
- [x] `components/audio_codec/` — Speex 코덱 컴포넌트
  - `speex/` — xiph/speex 원본 (git submodule, pristine 유지)
  - `CMakeLists.txt` — ESP-IDF 빌드용 래퍼 (협대역 전용 소스만 선별)
  - `audio_codec.h` / `audio_codec.c` — encode/decode 얇은 래퍼
- [x] `main/fsm.c` / `main/fsm.h` — 단말 전체 통합 FSM (설계는 [docs/fsm-design.md](fhss-ota-radio/docs/fsm-design.md) 참고), `main.c`에서 `fsm_init()`으로 wiring
  - **메뉴 기반 수신 모드 게이팅**: 메뉴는 `MENU_COMM`(음성, 기본)/`MENU_IDLE`(뮤트)/`MENU_OTA`(OTA 대기) 3-way. 로터리 엔코더 클릭 이벤트(`EV_MENU_SELECT_COMM`/`IDLE`/`OTA`)로만 전환되며, 음성 송수신·OTA 수신/적용 중에는 이 전이가 정의돼 있지 않아 메뉴 변경이 SW적으로 불가능함
  - 부수 효과: 음성 통화 중 OTA가 강제로 끼어드는 이전 전이(`TX_AUDIO`/`RX_AUDIO` → `OTA_RECEIVING` on `EV_OTA_START`)는 폐기 — 이제 `MENU_OTA`에서만 OTA 수신이 유효함
  - **3-way 확장(2026-08-11)**: 기존 `MENU_IDLE`(통신 대기)을 `MENU_COMM`으로 개명하고, `MENU_IDLE`을 PTT/수신 전이가 없는 완전히 새로운 **뮤트** 상태로 재정의. `on_enter_menu_idle()`(옛 이름, 오디오 태스크 정리 담당)은 `on_enter_menu_comm()`으로 개명, 새 `on_enter_menu_idle()`은 화면만 다시 그리는 최소 구현(뮤트 상태로는 TX_AUDIO/RX_AUDIO 진입 자체가 안 되니 정리할 태스크가 없음). `FSM_EVENT_SYNC_LOST` 안전장치의 복귀 목적지도 `MENU_COMM`으로 변경(뮤트가 아니라 정상 통신 대기로 복귀해야 함)
- [x] `components/display_ui/` — 0.96" I2C OLED(SSD1306, 128x64) 상태 표시 컴포넌트, **실기기 테스트 완료**
  - `display_ui_config.h` — I2C 핀 SDA=GPIO21/SCL=GPIO20(2026-08-11 브레드보드 재구성 배선, 최종 확정 전까지 바뀔 수 있음)·주소·해상도 매크로
  - `display_ui.h` / `display_ui.c` — `driver/i2c_master.h`(ESP-IDF v5.2+ 신규 API) 기반 SSD1306 드라이버
  - `font8x8_basic.h` — 공개도메인 8x8 비트맵 폰트 ([dhepper/font8x8](https://github.com/dhepper/font8x8) 원본과 바이트 단위 대조 검증)
  - 공개 API(레거시, 물리/가로 좌표): `display_ui_init()`, `display_ui_clear()`, `oled_update_text(row, text)`, `oled_update_text_fmt(row, fmt, ...)`
  - **회전 메뉴 화면(2026-08-11)**: `display_ui_draw_menu(selected, hovered)` 신규 API — 배선(SDA/SCL) 그대로 두고 화면 내용만 좌측 90도 회전(세로, 논리 64x128)해서 COMM/IDLE/OTA 3항목 메뉴를 그림. SSD1306엔 진짜 90도 회전 명령이 없어(A0/A1·C0/C8은 0/180도 반전만 지원) 프레임버퍼에 좌표 변환(`px=ly, py=HEIGHT-1-lx`, 실기기로 방향 확인 후 확정)으로 직접 그린 뒤 8페이지 전체 flush. 텍스트는 기존 8x8 폰트를 2배 확대(픽셀 더블링)해서 크고 굵게 — 새 폰트 에셋 없이 구현. `selected` 항목은 배경/글자색 반전, `hovered` 항목은 흰색(반전과 겹치면 검은색) 테두리
  - **상태 메시지 한 줄(2026-08-12)**: 메뉴 아래에 `display_ui_set_status()`/`display_ui_set_status_animated()`(250ms 마침표 애니메이션, 내부 esp_timer) 추가 — 지금 이 모드가 뭘 하는 중인지 표시(HOLD PTT / TX... / RX... / MUTED·PTT:TEST / WAIT...). 여유 공간 확보 위해 메뉴 항목을 28px→24px로 살짝 축소. 자세한 내용은 [components/display_ui/README.md](fhss-ota-radio/components/display_ui/README.md)
- [x] `components/ptt_button/` — PTT 버튼 디바운스 컴포넌트
  - `ptt_button_config.h` — 핀(GPIO1, 2026-08-11 브레드보드 재구성 배선)/active level/디바운스 파라미터
  - `ptt_button.h` / `ptt_button.c` — 폴링 기반 디바운스(ISR 미사용), 콜백/폴링 API 제공
  - 공개 API: `ptt_button_init()`, `ptt_button_set_callback(cb, ctx)`, `ptt_button_is_pressed()`
- [x] `components/rotary_encoder/` — 메뉴 선택용 로터리 엔코더(A/B/SW) 컴포넌트
  - `rotary_encoder_config.h` — 핀 A/B/SW(모듈 라벨 S1/S2/KEY)=GPIO2/42/41(placeholder, 2026-08-11 재배정). GPIO1은 ptt_button과 겹쳐서 사용 금지
  - `rotary_encoder.h` / `rotary_encoder.c` — quadrature 폴링 디코딩 + SW 디바운스, 커서 이동은 3-way 순환(`COMM -> IDLE -> OTA -> COMM`)
  - 공개 API: `rotary_encoder_init()`, `rotary_encoder_set_cursor_callback()`, `rotary_encoder_set_select_callback()`, `rotary_encoder_get_cursor()`
- [x] `components/audio_io/` — I2S 마이크(INMP441)/스피커(MAX98357A) 입출력 + audio_codec 연결, **마이크 캡처 실기기 테스트 완료, 앰프 배선/테스트 진행 중**
  - `audio_io_config.h` — 마이크(WS=4,BCLK=5,SD=6, L/R=3V3 고정=우채널, 2026-08-11 재배정)/스피커(LRC=46,BCLK=3,DIN=8,GAIN=18,SD=17, 2026-08-11 재배정 — GPIO9~14를 CC1101 SPI+GDO0+GDO2용으로 통째로 비움) 핀 설정
  - 마이크(RX)=`I2S_NUM_0`, 스피커(TX)=`I2S_NUM_1` 별도 포트 고정 배정 (재설정 없이 동시 존재)
  - 공개 API: `audio_io_init()`, `audio_io_capture_encode(out, cap)`, `audio_io_decode_play(data, len)`, `audio_io_speaker_enable()`/`audio_io_speaker_disable()`, `audio_io_play_beep()` — 내부에서 `audio_codec_encode/decode` 호출
  - **스피커 채널은 지연 활성화**: 마이크와 달리 `audio_io_init()`에서 채널만 만들고 enable 안 함 — 실제 재생 시점(`RX_AUDIO` 진입, 또는 삐빅음 재생 직전)에만 `audio_io_speaker_enable()`로 켬. 켜둔 채 한 번도 안 쓰면 GDMA TX 인터럽트가 NULL 컨텍스트로 불려 재부팅되는 문제가 실기기에서 확인돼 수정함(2026-08-10)
  - **GAIN/SD GPIO 제어 추가**: GAIN은 항상 HIGH(VDD)=6dB 고정(GPIO로 저항 없이 가능한 값 중 최소), SD는 `audio_io_speaker_enable/disable()`에서 HIGH(왼쪽 채널 출력)/LOW(완전 꺼짐)로 제어
  - **PTT 테스트용 알림음**: `RX_AUDIO`(수신 재생) 미구현 상태에서 앰프 동작 확인용으로 `audio_io_play_beep()` 추가 — A5(880Hz)→D6(1175Hz) 2음. 볼륨은 GAIN 6dB + 소프트웨어 진폭(`AUDIO_IO_BEEP_AMPLITUDE`=500/32767, 약 1.5%)로 이중으로 최소화
  - **`LOOPBACK_ENABLE`(임시, 기본 꺼짐)**: 켜면 MENU_IDLE에서 PTT로 mic->Speex 인코딩/디코딩 왕복->1초 뒤 스피커 재생하는 loopback 테스트(`main/fsm.c`의 `mic_test_task`)가 활성화됨 — INMP441 실배선 확인용. INMP441은 아날로그 게인이 없어 일반 발화 거리에서 캡처 진폭이 원래 작아서(실측 peak 846/32767) `audio_io_decode_play_scaled()`로 소프트웨어 gain(16배) 증폭 후 재생. 진단 과정은 로컬 `troubleshoot/mic_loopback_test-inmp441_low_amplitude.md` 참고(git 비관리)
- [x] **`display_ui`/`ptt_button`/`rotary_encoder`/`audio_io` → FSM wiring 완료** (`main/fsm.c`)
  - 부팅 시(`on_enter_boot_init()`) 네 컴포넌트 `init()` + 콜백 등록: `ptt_button` press/release → `FSM_EVENT_PTT_PRESS/RELEASE`, `rotary_encoder` 클릭 → `FSM_EVENT_MENU_SELECT_COMM/IDLE/OTA`, 로터리 회전 → `display_ui_draw_menu()`로 흰 테두리(hover)만 갱신(FSM 이벤트 아님), `audio_codec_init()`/`audio_io_init()`
  - `on_enter_menu_comm`/`on_enter_menu_idle`/`on_enter_menu_ota`에서 `display_ui_draw_menu(selected, hovered)`로 메뉴 화면 갱신 — `fsm.c`의 `menu_item_from_fsm_state()`/`menu_item_from_rotary()`가 FSM/로터리 enum을 `display_ui_menu_item_t`로 변환
  - `on_enter_tx_audio()`: **`fsm_task` 안에서 블로킹으로** 스피커 켜고 `audio_io_play_beep()` 삐빅음 재생 후 다시 끔(앰프 테스트용) → 그 다음에야 `tx_audio_task`(마이크 20ms 캡처 루프, 스택 8192) 생성. 삐빅음을 태스크 안이 아니라 `fsm_task`에서 블로킹으로 끝까지 재생하는 이유: PTT를 삐빅음 재생 중(~210ms 이내)에 떼면 `vTaskDelete()`가 스피커 disable 전에 태스크를 강제 종료시켜 삐빅음이 안 끝나고 계속 재생되는 버그가 있었음 — `fsm_task`는 이벤트를 하나씩 순서대로 처리하니 삐빅음이 끝나야 `PTT_RELEASE`를 처리해서 이 경쟁 상태를 없앰(진단 과정은 로컬 `troubleshoot/on_enter_tx_audio-beep_fix.md` 참고, git 비관리). PTT_RELEASE로 `MENU_COMM` 재진입 시(`on_enter_menu_comm()`) `tx_audio_task` 정리. **실기기 검증 완료** — PTT 눌러서 `MENU_COMM → TX_AUDIO` 실제 전이 확인
  - `on_enter_rx_audio()`: `audio_io_speaker_enable()`로 스피커 켠 뒤, 오디오 프레임 전용 큐(`fsm_post_rx_audio_frame(data, len)`, `main/fsm.h` 신규 API)에서 프레임을 꺼내 `audio_io_decode_play()`로 재생하는 태스크(스택 8192) 시작, `on_enter_menu_comm()`에서 태스크 정리 + `audio_io_speaker_disable()` — `fsm_event_t`(페이로드 없는 enum)와 별개 큐로 데이터 전달
  - `FSM_EVENT_RX_DONE`은 무음 타임아웃 1초(`FSM_RX_AUDIO_IDLE_TIMEOUT_MS`)로 확정 — `rx_audio_task`가 1초간 새 프레임 없으면 스스로 이벤트를 올리고 종료 (`rf_transport` 없어 아직 실측 검증은 안 됨)
  - **미정으로 명시해둔 것**: `fsm_post_rx_audio_frame()`을 실제 호출할 `rf_transport`가 아직 없어 큐가 항상 비어있음 (`docs/fsm-design.md` 결정 이력 2026-08-06 참고)
  - **의도적으로 안 채운 것**: `ota_*`는 그대로 TODO — `rf_transport`가 없어 실제 수신 트리거가 없음
  - **`FHSS_SYNC` 상태/`SYNC_ACQUIRED` 이벤트 제거(2026-08-10)**: 브로드캐스트 설계로 확정(PTT 누른 쪽이 정해진 시작 채널로 먼저 송신 후 시드 기반 호핑, 받는 쪽은 그 수신 시점 기준으로 추종) — 별도 "동기 획득 대기" 상태가 불필요해져 `FSM_STATE_FHSS_SYNC`/`FSM_EVENT_SYNC_ACQUIRED`를 `fsm.h`/`fsm.c`에서 삭제하고 `BOOT_INIT`이 곧바로 기본 메뉴로 전이하도록 변경. **이제 PTT를 누르면 FSM이 실제로 `TX_AUDIO`로 전이한다** (예전엔 `FHSS_SYNC`에 멈춰 unhandled로 무시됐음)
  - `FSM_EVENT_SYNC_LOST`는 전역 안전장치 이벤트로 유지 — 목적지는 `MENU_COMM`(정상 통신 대기, 뮤트인 `MENU_IDLE` 아님). 무선 계층(`rf_transport`/`fhss_core`)이 홉 추종 실패를 판단하면 이 이벤트로 강제 복귀시키는 용도(팀5의 `fhss_sync_state` 모듈이 판정 로직 후보, 아직 미완성)
  - **알려진 제약**: `rf_transport`가 없어 `TX_AUDIO`에서 캡처한 프레임을 실제로 보낼 곳도, `RX_AUDIO`가 받을 실제 프레임도 없음 — 그래서 이 wiring은 컴파일/개별 컴포넌트 단위 검증까지만 가능하고, `rf_transport` 생기기 전까지 실기기 end-to-end 테스트는 불가
- [x] `components/ota_client/` — OTA 세션/청크 검증/플래시 기록 컴포넌트 (팀2, 별도 브랜치에서 병합됨) — `rf_transport`(무선 송수신)가 아직 없어 실제 동작은 불가, 역할 분리만 잡혀있는 상태 (자세한 내용은 [components/ota_client/README.md](fhss-ota-radio/components/ota_client/README.md))
  - **OTA 스캔 ACK 구조 선반영(2026-08-12)**: `ota_discover_packet.h/.c` 추가 — Qt 앱의 `OTA_DISCOVER`(2바이트) 수신 시 `MENU_OTA`이면 `device_id`+펌웨어 버전을 담은 `OTA_DISCOVER_ACK`(6바이트)를 준비(`main/fsm.c`의 `FSM_EVENT_OTA_DISCOVER_RX`, 상태 전이 없음). 실제 RF 송수신은 여전히 TODO
- [x] `components/status_led/` — 온보드 WS2812 RGB LED(GPIO38, `led_strip` managed component) 상태 표시 (디버그용)
  - `main/fsm.c`의 `on_ptt_event()`에 직접 연결 — FSM 처리 결과를 기다리지 않고 GPIO 디바운스만 통과하면 바로 켜짐/꺼짐 (FSM 전이표 변경과 무관하게 동작)
- [x] `components/device_id/` — 기기 고유 식별자(eFuse base MAC 뒤 3바이트, `DEVICE_ID_LEN`) — 자세한 배경은 위 "기기 고유 식별자" 섹션 참고
  - `on_enter_boot_init()`에서 `device_id_get_hex()`로 부팅 시 로그 한 번 찍음 — OTA ACK 등 실제 사용처는 `rf_transport` 생기면 연결 예정
- [x] `components/fhss_core/` — `fhss_sync_packet.c/h`(동기 패킷 encode/decode, big-endian 13바이트 와이어 포맷) 구현됨 (팀5, 별도 브랜치에서 병합됨)
  - `fhss_hop_sequence.c/h`는 아직 빈 스텁 — 호핑 시퀀스 계산 로직 미구현
- [ ] CC1101 저수준 SPI(`rf_transport`), `fhss_hop_sequence` 실구현, `fhss_sync_state`(SEARCHING/LOCKED 판정, 헤더 비어있어 빌드 안 됨)는 아직 미완성 — 이게 없어서 FSM wiring도, `ota_client`도 실기기에서는 검증 못 하는 상태

## 담당
팀원1 (오디오/OLED), 팀원2 (OTA/부트로더), 팀원5 (FHSS)
