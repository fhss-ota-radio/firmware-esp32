# PR: GDO0 기반 FHSS 무선 동기화 통합

## PR 제목

`feat: integrate GDO0-based synchronized FHSS hopping`

## 작업 목적

- CC1101 GDO0 인터럽트로 sync word 검출 시각을 마이크로초 단위로 기록한다.
- SYNC 패킷의 `slot_number`와 GDO0 timestamp를 결합해 RX의 슬롯 시간축을 설정한다.
- TX와 RX가 동일한 슬롯 번호와 홉 채널을 실제 하드웨어에서 추종하도록 한다.
- 동기 획득과 상실 이벤트를 상위 FSM에 전달한다.
- `main.c`의 임시 Smoke Test 루프를 서비스 컴포넌트로 분리한다.

## 주요 변경 사항

### RF Transport

- CC1101 GDO0 입력 GPIO와 상승 에지 인터럽트 지원
- ISR에서 `esp_timer_get_time()` timestamp를 FreeRTOS 큐에 전달
- `rf_transport_start_receive()` 추가
- `rf_transport_wait_rx_timestamp()` 추가
- ISR에서는 SPI, 로그 및 FHSS 계산을 수행하지 않도록 분리

### FHSS Core

- `fhss_slot_scheduler` 추가
  - 기준 slot/time 저장
  - 현재 슬롯과 슬롯 시작 시각 계산
  - 다음 채널 전환 시각 계산
  - 초기화, 미동기, 기준 이전 시각과 overflow 오류 처리
- `fhss_sync_controller` 추가
  - 첫 SYNC로 RX scheduler bootstrap
  - 이후 예상 시각과 실제 GDO0 시각 비교
  - 유효 수신 시 기준 시각 보정
  - `SYNC_LOST` 시 scheduler 기준 제거
- Timing Window의 early 경계가 `late_margin_us`를 사용하던 오류 수정

### FHSS Service/FSM

- `fhss_service` 컴포넌트 추가
- 내부 상태:
  - `STOPPED`
  - `SEARCHING`
  - `SYNCHRONIZING`
  - `TRACKING`
  - `TRANSMITTING`
- TX가 300 ms 고정 슬롯에 맞춰 SYNC 패킷 송신
- RX가 채널 검색 후 SYNC 기준으로 synchronized hopping 수행
- 연속 3회 유효 수신 시 `SYNC_ACQUIRED`
- 연속 5회 MISS 시 `SYNC_LOST`

### Main FSM

- `FSM_EVENT_SYNC_ACQUIRED` 추가
- ACQUIRED는 관측 로그로 처리하며 별도 전역 상태 전이는 만들지 않음
- LOST는 기존 정책대로 `MENU_IDLE` 안전 상태로 복귀
- `main.c`는 설정, `fhss_service_init()`, `fhss_service_start()` 중심으로 단순화

### Watchdog 안정화

- `ptt_button` 5 ms polling과 `rotary_encoder` 2 ms polling이 0 tick으로 변환되지 않도록 최소 1 tick 보장
- 전역 FSM 활성 상태에서 IDLE task watchdog 재발이 없음을 확인

## 동기화 흐름

```text
TX slot 시작
    ↓
SYNC 패킷 송신(slot_number 포함)
    ↓ RF
RX GDO0 상승 에지
    ↓ rx_timestamp_us
SYNC 패킷 decode
    ↓
reference_slot = packet.slot_number
reference_time = rx_timestamp_us - sync_offset_us
    ↓
다음 slot/channel 계산
    ↓
슬롯 시작 5 ms 전에 CC1101 채널 변경
```

## 상태 흐름

```text
SEARCHING
    ↓ 첫 유효 SYNC
SYNCHRONIZING
    ↓ 연속 3회 유효 수신
TRACKING + SYNC_ACQUIRED
    ↓ 연속 5회 MISS
SEARCHING + SYNC_LOST
```

## 주요 설정

| 항목 | 값 |
|---|---|
| TX 보드 | COM3 |
| RX 보드 | COM5 |
| GDO0 연결 | CC1101 GDO0 → ESP32-S3 GPIO18 |
| 슬롯 길이 | 300,000 us |
| 채널 전환 guard | 5,000 us |
| 홉 채널 | `CHANNR {0, 10, 20}` |
| SYNC 획득 임계값 | 연속 3회 |
| SYNC 상실 임계값 | 연속 5회 |
| RX 검색 체류 시간 | 137 ms |

## 실기기 검증 결과

### TX GDO0 timestamp

```text
slot=71 timestamp=21457625
slot=72 timestamp=21757627  delta=300002 us
slot=73 timestamp=22057626  delta=299999 us
slot=74 timestamp=22357625  delta=299999 us
```

- TX 슬롯 간격: `300000 ± 2 us`
- 채널 0, 10, 20 순환 확인

### RX synchronized hopping

```text
SYNC RX: state=TRACKING slot=307 channel=10 error=-2 us
SYNC RX: state=TRACKING slot=308 channel=20 error=1 us
SYNC RX: state=TRACKING slot=309 channel=0  error=-3 us
```

- TX/RX slot 일치
- TX/RX channel 일치
- 일반적인 timing error: 약 `-3 ~ +1 us`
- 상실/재획득 시험 포함 관측 범위: 약 `-9 ~ +5 us`

### 상실 및 재획득

COM3 TX를 RTS로 약 3초 reset했다.

```text
FHSS service event: SYNC_LOST
SYNC RX: state=SEARCHING slot=5 channel=20
SYNC RX: state=SYNCHRONIZING slot=6 channel=0
SYNC RX: state=SYNCHRONIZING slot=7 channel=10
FHSS service event: SYNC_ACQUIRED
fsm: FHSS synchronization acquired in state MENU_IDLE
SYNC RX: state=TRACKING slot=8 channel=20
```

- `TRACKING → SEARCHING` 성공
- TX 재부팅 후 자동 재획득 성공
- `SYNC_ACQUIRED / SYNC_LOST` main callback 전달 성공
- 전역 FSM 활성 상태에서 watchdog 재발 없음

## 빌드 검증

- `ninja -C build` 성공
- `fhss-ota-radio.bin` 생성 성공
- 최종 바이너리 크기: `0x4f7c0`
- 최소 앱 파티션 여유: 69%
- `git diff --check` 통과

## 리뷰 포인트

- [ ] GDO0 ISR이 timestamp 저장과 큐 전달만 수행하는가?
- [ ] ISR에 등록되는 `rf_transport_t` 주소가 서비스 수명 동안 유지되는가?
- [ ] TX가 GDO timestamp로 매번 기준을 재설정하지 않고 고정 슬롯 시계를 유지하는가?
- [ ] 첫 SYNC bootstrap과 이후 timing window 검증 흐름이 구분되는가?
- [ ] scheduler의 기준 이전 시각과 overflow 처리가 충분한가?
- [ ] SYNCHRONIZING 중 MISS 발생 시 SEARCHING으로 안전하게 복귀하는가?
- [ ] ACQUIRED는 관측 이벤트, LOST는 전역 안전 전이라는 정책이 기존 FSM 설계와 일치하는가?
- [ ] GPIO18이 다른 실제 보드 기능과 충돌하지 않는가?

## 영향 범위와 의존성

- 기준 브랜치: `feature/rf-transport-cc1101`
- 작업 브랜치: `feature/fhss-radio-sync-integration`
- 커밋:
  - `8695cae` — slot scheduler와 sync controller
  - `3ee35f9` — GDO0 synchronized FHSS service
  - `028c968` — polling watchdog 안정화와 검증 문서
- 선행 기능:
  - CC1101 SPI/RF transport
  - SYNC packet encode/decode
  - Hop Sequence
  - Timing Window
  - Sync State

## 제외 범위

- 실제 음성 프레임 송수신
- OTA 데이터와 FHSS 스케줄의 모드 전환
- ACK 및 재전송
- 장시간 clock drift 통계
- slot number wrap-around 정책
- GDO0 timestamp와 FIFO packet sequence의 강한 상관 검증
- `sync_offset_us` 실측 보정

## PR 상태

- 원격 브랜치 push 완료
- GitHub Pull Request는 아직 생성하지 않음
