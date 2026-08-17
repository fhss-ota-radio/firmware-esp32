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

**⚠️ `CONFIG_FREERTOS_HZ` 확인 필수** — `100`(기본값)이면 `rotary_encoder`/`ptt_button` 폴링이 CPU 99% busy-loop 버그로 재현됨.

- 기존 로컬 `sdkconfig` 있음: `CONFIG_FREERTOS_HZ=1000`인지 확인 (`idf.py menuconfig` > Component config > FreeRTOS > Kernel > Tick rate)
- 새로 clone: `sdkconfig.defaults` 자동 적용, 조치 불필요

**⚠️ N8R8 보드는 별도 명령 필요(N16R8/N8R8 혼용 중)** — 팀 기본값은 N16R8(Flash 16MB)이라 `sdkconfig.defaults`에 그대로 들어있음. **VSCode 빌드 버튼이나 옵션 없는 `idf.py build`는 그대로 N16R8로 빌드됨(대부분은 조치 불필요).** 부품 수급 문제로 N8R8(Flash 8MB)을 쓰는 사람만 아래처럼 오버레이 파일을 추가로 얹어야 함. Flash 총 용량이 실제 칩보다 큰 값으로 빌드된 바이너리를 플래시하면 아주 초기 단계(OLED 등 아무 주변장치도 못 켜는 시점)에 죽는다(2026-08-17 N8R8 실기기에서 확인). `partitions.csv`는 두 보드가 공유하므로 이 오버레이 하나면 충분함:

```bash
# N8R8 (Flash 8MB) — 부품 수급 문제로 병행 사용 중인 개발용 보드만 필요
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.n8r8" build
```

**N16R8 ↔ N8R8을 오가며 빌드할 땐 반드시 먼저 `sdkconfig` 자체를 지울 것** — `idf.py fullclean`은 `build/`만 지우고 `sdkconfig`는 그대로 남기므로, 이전 보드용 Flash 크기가 계속 남아있게 됨(2026-08-17 N8R8에서 실제로 이 문제로 재현됨: `fullclean` 후 재빌드했는데도 esptool 로그에 `--flash-size 16MB`가 그대로 찍힘). `sdkconfig`까지 지워야 `SDKCONFIG_DEFAULTS`가 다시 반영됨:

```bash
rm -f sdkconfig && idf.py fullclean   # build/뿐 아니라 sdkconfig도 지워야 함
```

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

## 컴포넌트 구조
```
firmware-esp32/
├── main/                  # 통합 진입점 (컴포넌트 wiring, 전역 FSM)
├── components/
│   ├── audio_io/          # 팀원1 — I2S 마이크/스피커
│   ├── audio_codec/       # 팀원1 — Speex 인코더/디코더
│   ├── display_ui/        # 팀원1 — OLED 상태 표시
│   ├── ptt_button/        # 팀원1 — PTT 디바운스, EV_PTT_PRESS/RELEASE
│   ├── rotary_encoder/    # 팀원1 — 메뉴 커서, EV_MENU_SELECT_COMM/IDLE/OTA
│   ├── status_led/        # 팀원1 — 온보드 RGB LED 상태 표시 (디버그용)
│   ├── device_id/         # 팀원1 — 기기 고유 식별자(MAC 뒤 3바이트)
│   ├── ota_client/        # 팀원2 — OTA 세션/청크 검증, 파티션 기록
│   ├── ota_protocol/      # 팀원2+3,4 — 공유 와이어 포맷 (submodule)
│   ├── ota_rf_bridge/     # 팀원2 — RF 수신 패킷 → ota_client 큐 어댑터
│   ├── fhss_core/         # 팀원5 — 호핑 시퀀스, 동기화 로직
│   ├── fhss_service/      # 팀원5 — fhss_core+rf_transport 상위 서비스
│   └── rf_transport/      # 팀원5(+2) — CC1101 SPI 드라이버
└── docs/
```

## 현재 구현 현황

### 팀1 (오디오/UI) — 실기기 검증 완료
- `audio_io`/`audio_codec`: I2S 마이크(INMP441)/스피커(MAX98357A) + Speex 인코딩/디코딩
- `display_ui`: OLED 세로 회전 3-way 메뉴(COMM/IDLE/OTA), 상태줄(정적/점 애니메이션/스크롤), OTA 진행률(%) 표시
- `ptt_button`/`rotary_encoder`: 디바운스/커서 폴링, FSM 연결 완료
- `status_led`: PTT=흰색(실제 캡처로 이어질 때만), `RX_AUDIO`=하늘색, `ERROR`=빨간 점멸
- `device_id`: eFuse MAC 뒤 3바이트, OTA ACK 페이로드로 사용 중
- `main/fsm.c`: 전체 상태 전이 오케스트레이션(설계는 [docs/fsm-design.md](fhss-ota-radio/docs/fsm-design.md)) — ERROR 상태 + `EV_RETRY` 재진입 가드, OTA 진행률/실패 OLED 연동까지 완료

### 팀5 (FHSS/무선) — 컴포넌트 구현됨, FSM 미연결
- `rf_transport`: CC1101 SPI 드라이버. 1MHz에서 인접 배선(GPIO9~12) 크로스토크로 통신 실패, 10kHz는 정상 동작 확인(별도 진단 브랜치) — 실제 연결 시 클럭값 참고 필요
- `fhss_core`/`fhss_service`: 호핑 시퀀스·동기화 로직 구현, `examples/fhss_sync_test`로 별도 검증
- `main/fsm.c`에는 아직 미연결(`rf_transport_init()` 호출 지점 없음) — 다음 단계

### 팀2 (OTA) — 프로토콜/세션 로직 구현됨, FSM 일부 연결
- `ota_protocol`(submodule): OTA_DISCOVER/START/DATA/END/ACK/NACK 와이어 포맷 전부 정의
- `ota_client`/`ota_rf_bridge`: 세션 검증, 청크 재조립, 파티션 기록까지 구현
- `fsm_ota_event_callback()`으로 진행률(%)/검증 실패를 OLED에 표시
- **알려진 이슈**: 검증 성공 후 실제 `esp_restart()` 호출이 없어 재부팅이 안 됨 — 팀2 확인 필요

### 공통 미해결
- [ ] `rf_transport`를 `main/fsm.c`에 연결(라디오 초기화, TX/RX 프레임 송수신, 모드 전환)
- [ ] OTA 검증 성공 후 재부팅 로직 (위 참고)

## 담당
팀원1 (오디오/OLED), 팀원2 (OTA/부트로더), 팀원5 (FHSS)
