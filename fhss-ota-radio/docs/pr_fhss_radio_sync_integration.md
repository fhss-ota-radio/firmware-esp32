# PR: GDO0 기반 FHSS 무선 동기화 통합

## PR 제목

`feat: integrate GDO0-based synchronized FHSS hopping`

## 작업 목적

- CC1101 GDO0 인터럽트로 sync word 검출 시각을 마이크로초 단위로 기록한다.
- SYNC 패킷의 `slot_number`와 GDO0 timestamp를 결합해 RX의 슬롯 시간축을 설정한다.
- TX와 RX가 동일한 슬롯 번호와 홉 채널을 실제 하드웨어에서 추종하도록 한다.
- 동기 획득과 상실을 FHSS 내부 FSM에서 판정한다.
- 고정 TX/RX Smoke Test를 제품 `main`에서 독립 example로 분리한다.
- 최신 `develop`과 `fsm-design.md`의 애플리케이션 FSM을 수정하지 않는다.

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

### Develop FSM 경계

- `main/fsm.c`, `main/fsm.h`, `main/main.c`를 변경하지 않음
- `SYNC_ACQUIRED`는 `fsm-design.md` 결정대로 FHSS 내부 관측 이벤트로만 유지
- `SYNC_LOST`를 제품 FSM에 전달하는 연결은 session 기반 role 전환 API와 함께 후속 구현
- 고정 역할 TX/RX 하드웨어 테스트는 `examples/fhss_sync_test`로 이동
- `ptt_button`, `rotary_encoder`, audio, display, OTA 등 타 팀 컴포넌트를 변경하지 않음

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
fhss_sync_test: SYNC_LOST
SYNC RX: state=SEARCHING slot=5 channel=20
SYNC RX: state=SYNCHRONIZING slot=6 channel=0
SYNC RX: state=SYNCHRONIZING slot=7 channel=10
fhss_sync_test: SYNC_ACQUIRED
SYNC RX: state=TRACKING slot=8 channel=20
```

- `TRACKING → SEARCHING` 성공
- TX 재부팅 후 자동 재획득 성공
- example callback에서 `SYNC_ACQUIRED / SYNC_LOST` 확인
- 제품 전역 FSM과 분리된 상태로 검증

## 빌드 검증

- 제품 루트 `ninja -C build` 성공
- 독립 `examples/fhss_sync_test` 빌드 성공
- example 바이너리 크기: `0x2cb10`
- example 앱 파티션 여유: 83%
- `git diff --check` 통과

## 리뷰 포인트

- [ ] GDO0 ISR이 timestamp 저장과 큐 전달만 수행하는가?
- [ ] ISR에 등록되는 `rf_transport_t` 주소가 서비스 수명 동안 유지되는가?
- [ ] TX가 GDO timestamp로 매번 기준을 재설정하지 않고 고정 슬롯 시계를 유지하는가?
- [ ] 첫 SYNC bootstrap과 이후 timing window 검증 흐름이 구분되는가?
- [ ] scheduler의 기준 이전 시각과 overflow 처리가 충분한가?
- [ ] SYNCHRONIZING 중 MISS 발생 시 SEARCHING으로 안전하게 복귀하는가?
- [ ] ACQUIRED가 FHSS 내부에만 머물러 `fsm-design.md` 정책과 일치하는가?
- [ ] 고정 역할 테스트가 제품 `main`과 완전히 분리됐는가?
- [ ] GPIO18이 다른 실제 보드 기능과 충돌하지 않는가?

## 영향 범위와 의존성

- 기준 브랜치: `feature/rf-transport-cc1101`
- 작업 브랜치: `feature/fhss-radio-sync-integration`
- 커밋:
  - `8695cae` — slot scheduler와 sync controller
  - `3ee35f9` — GDO0 synchronized FHSS service
  - `028c968` — 실기기 검증 중 polling watchdog 원인 확인
  - `a4bee5b` — 제품 FSM 침범 제거 및 독립 example 분리
- 선행 기능:
  - CC1101 SPI/RF transport
  - SYNC packet encode/decode
  - Hop Sequence
  - Timing Window
  - Sync State

## 제외 범위

- 실제 음성 프레임 송수신
- 제품 FSM의 `TX_AUDIO`/`RX_AUDIO` 기반 동적 role 시작·정지 adapter
- `FSM_EVENT_SYNC_LOST` 전달 연결
- OTA 데이터와 FHSS 스케줄의 모드 전환
- ACK 및 재전송
- 장시간 clock drift 통계
- slot number wrap-around 정책
- GDO0 timestamp와 FIFO packet sequence의 강한 상관 검증
- `sync_offset_us` 실측 보정

## PR 상태

- 원격 브랜치 push 완료
- GitHub Draft Pull Request #24 생성
