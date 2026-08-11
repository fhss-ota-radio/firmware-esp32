# FHSS Radio Sync 통합 및 실기기 검증

## 1. 목적

CC1101 GDO0 인터럽트의 수신 시각과 SYNC 패킷의 슬롯 번호를 결합해 두 ESP32-S3가 동일한 슬롯과 채널을 따라가도록 구현한다.

구현 범위:

- GDO0 interrupt와 `rx_timestamp_us`
- `fhss_slot_scheduler`
- SYNC 패킷 기반 RX 기준 slot/time 설정
- TX/RX synchronized hopping
- `SYNC_ACQUIRED`, `SYNC_LOST`의 main FSM 연결

## 2. 하드웨어 설정

| CC1101 | ESP32-S3 |
|---|---|
| SCLK | GPIO12 |
| MOSI | GPIO11 |
| MISO | GPIO13 |
| CS | GPIO14 |
| GDO0 | GPIO18 |
| GND | GND |

GDO0는 CC1101 `IOCFG0=0x06` 설정을 사용한다. sync word가 송신 또는 검출될 때 상승하고 패킷 종료 시 하강한다. 상승 에지에서 `esp_timer_get_time()`을 기록한다.

## 3. 구조

```text
CC1101 GDO0 ISR
    ↓ rx_timestamp_us
rf_transport
    ↓ packet + timestamp
fhss_sync_controller
    ├─ fhss_core: packet/timing/sync/hop 판정
    └─ fhss_slot_scheduler: 기준 slot/time 및 다음 전환 시각
    ↓
fhss_service
    ├─ SEARCHING
    ├─ SYNCHRONIZING
    ├─ TRACKING
    └─ TRANSMITTING
    ↓ SYNC_ACQUIRED / SYNC_LOST
main/fsm
```

ISR에서는 timestamp 저장과 FreeRTOS 큐 전달만 수행한다. SPI FIFO 읽기, 로그, FHSS 계산과 채널 변경은 서비스 태스크에서 수행한다.

## 4. 시간 기준

TX는 최초 기준 시각에서 300 ms 고정 슬롯을 유지한다. GDO0 timestamp는 측정에만 사용하며 TX 기준을 다시 설정하지 않는다.

RX는 첫 유효 SYNC의 다음 값을 기준으로 설정한다.

```text
reference_slot = sync_packet.slot_number
reference_time = rx_timestamp_us - sync_offset_us
```

이후 예상 SYNC 시각과 실제 GDO0 시각의 차이를 timing error로 계산한다. 허용 범위 안의 패킷은 최신 관측값으로 RX 기준을 보정한다.

## 5. 동작 흐름

### TX

```text
다음 슬롯 채널 선택
→ 슬롯 시작까지 대기
→ SYNC 패킷 송신
→ GDO0 timestamp 기록
→ 다음 슬롯
```

### RX

```text
SEARCHING: 채널 0/10/20 순환 검색
→ 첫 SYNC 수신
→ slot + timestamp 기준 설정
→ SYNCHRONIZING
→ 연속 3회 유효 수신
→ SYNC_ACQUIRED / TRACKING
→ 연속 5회 MISS
→ SYNC_LOST / SEARCHING
```

## 6. 실제 검증 결과

검증 환경:

- COM3: TX
- COM5: RX
- 슬롯 길이: 300,000 us
- 채널 전환 guard: 5,000 us
- 홉 채널: `CHANNR {0, 10, 20}`
- 획득 임계값: 연속 3회
- 상실 임계값: 연속 5회

### TX GDO0 주기

```text
slot=71 timestamp=21457625
slot=72 timestamp=21757627  delta=300002 us
slot=73 timestamp=22057626  delta=299999 us
slot=74 timestamp=22357625  delta=299999 us
```

TX 슬롯 주기는 `300000 ± 2 us`로 측정됐다.

### RX synchronized hopping

```text
SYNC RX: state=TRACKING slot=307 channel=10 error=-2 us
SYNC RX: state=TRACKING slot=308 channel=20 error=1 us
SYNC RX: state=TRACKING slot=309 channel=0  error=-3 us
```

채널 0, 10, 20을 동일한 슬롯 순서로 연속 추종했고 일반적인 timing error는 약 `-3 ~ +1 us`였다.

### SYNC 상실과 재획득

COM3 TX를 RTS로 약 3초 reset했다.

```text
FHSS service event: SYNC_LOST
SYNC RX: state=SEARCHING slot=5 channel=20 error=0 us
SYNC RX: state=SYNCHRONIZING slot=6 channel=0 error=5 us
SYNC RX: state=SYNCHRONIZING slot=7 channel=10 error=-2 us
FHSS service event: SYNC_ACQUIRED
fsm: FHSS synchronization acquired in state MENU_IDLE
SYNC RX: state=TRACKING slot=8 channel=20 error=-6 us
```

상실 후 SEARCHING 복귀, TX 재부팅 후 기준 재설정, 연속 3회 수신 후 TRACKING 복귀를 확인했다.

## 7. 디버깅 중 발견한 문제

### ISR 객체 수명

`fhss_service_init()`이 임시 구조체의 radio 주소로 ISR을 등록하고 최종 객체로 복사해, ISR이 사라진 스택 주소를 참조했다. GDO0 상승 시 잘못된 queue handle로 FreeRTOS assert가 발생했다.

서비스의 최종 저장 위치에 `rf_transport`를 직접 초기화해 ISR 인자가 이동하지 않도록 수정했다.

### TX 슬롯 지연 누적

TX가 매번 GDO timestamp로 기준을 다시 설정하자 패킷 송신부터 sync word까지의 약 2.7 ms가 슬롯마다 누적돼 실제 간격이 약 302.7 ms가 됐다.

TX는 고정 기준 시계를 유지하고 GDO timestamp를 관측 용도로만 사용하도록 수정해 300 ms 주기를 복구했다.

### 주변장치 polling watchdog

FreeRTOS tick보다 짧은 2 ms와 5 ms polling 주기가 `pdMS_TO_TICKS()`에서 0 tick이 되면서 `rotary_encoder`, `ptt_button` 태스크가 IDLE 태스크를 굶겼다.

두 polling 태스크 모두 최소 1 tick을 지연하도록 변경했고 전역 FSM 활성 상태에서 watchdog이 재발하지 않았다.

### Timing Window early margin

기존 `fhss_timing_window`가 이른 수신 경계에 `early_margin_us` 대신 `late_margin_us`를 사용했다. 실제 timestamp 통합 전에 올바른 필드를 사용하도록 수정했다.

## 8. 현재 설정과 후속 작업

현재 소스는 RX 역할이며 전역 FSM 연결이 활성화돼 있다.

```c
#define FHSS_APP_ROLE FHSS_APP_ROLE_RX
#define FHSS_SYNC_TEST_ONLY 0
```

후속 작업:

- GDO0 timestamp와 FIFO packet의 sequence 결합 강화
- slot number wrap-around 정책
- 장시간 clock drift 측정
- 패킷 손실률과 채널별 통계
- `sync_offset_us` 실측 보정
- 오디오/OTA 데이터 프레임의 슬롯 배치
