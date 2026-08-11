# FHSS 무선 시간 동기화 구현 공유

> 팀 공유용 요약: CC1101의 GDO0 신호를 이용해 패킷이 실제 무선으로 감지된 시각을 기록하고, SYNC 패킷의 슬롯 번호와 결합해 두 보드가 같은 채널로 동시에 이동하도록 구현했다.

> **Develop FSM 반영 안내 (2026-08-11):** 아래 실기기 결과는 고정 TX/RX
> 테스트에서 얻은 것이다. 최신 `fsm-design.md`에 맞춰 테스트 하네스는
> `examples/fhss_sync_test`로 이동했고 제품 `main/fsm.*` 연결은 제거했다.
> `SYNC_ACQUIRED`는 FHSS 내부 관측 상태이며, 제품 FSM에는 후속 session
> adapter가 `SYNC_LOST`만 전달할 예정이다. PTT/로터리 watchdog 수정도 담당
> 팀 파일을 침범하지 않도록 이 브랜치에서 제외했다.

## 1. 왜 이 작업이 필요했나

이전 단계에서는 TX와 RX가 각각 채널 0, 10, 20을 순환했다. 세 채널에서 패킷이 수신되는 것은 확인했지만 두 보드의 시간 기준은 서로 달랐다.

```text
TX: 자기 타이머로 채널 이동
RX: 137 ms마다 채널을 순환 검색
```

이 방식은 FHSS 하드웨어 확인에는 충분하지만 실제 운용에는 적합하지 않다. RX가 우연히 TX와 같은 채널에 있을 때만 패킷을 받을 수 있기 때문이다.

이번 작업에서는 TX가 보내는 `slot_number`와 RX가 패킷을 감지한 정확한 시각을 사용해 RX의 시간 기준을 맞췄다.

## 2. 핵심 개념

### Slot

무선 동작을 일정 길이의 시간 구간으로 나눈 것이다. 현재 슬롯 길이는 300 ms다.

```text
slot 100 → channel 10
slot 101 → channel 20
slot 102 → channel 0
```

TX와 RX가 같은 slot 번호를 계산하면 `fhss_hop_sequence`가 같은 채널을 반환한다.

### SYNC 패킷

SYNC 패킷에는 다음 정보가 들어 있다.

- 프로토콜 magic/version
- sequence
- hop index
- `slot_number`

RX는 `slot_number`를 통해 “방금 받은 패킷이 TX의 몇 번째 슬롯인가”를 알 수 있다.

### GDO0 timestamp

SPI FIFO를 읽은 시각은 정확한 수신 시각이 아니다. FreeRTOS 스케줄링과 SPI 처리 지연이 포함되기 때문이다.

CC1101의 GDO0는 sync word를 검출하는 순간 상승한다. ESP32-S3는 이 상승 에지에서 `esp_timer_get_time()`을 호출해 마이크로초 timestamp를 저장한다.

```text
CC1101 sync word 검출
        ↓ GDO0 rising
ESP32 GPIO ISR
        ↓
rx_timestamp_us 저장
```

## 3. 배선

기존 SPI 배선에 GDO0 한 가닥을 추가했다.

| CC1101 | ESP32-S3 |
|---|---|
| SCLK | GPIO12 |
| MOSI | GPIO11 |
| MISO | GPIO13 |
| CS | GPIO14 |
| GDO0 | GPIO18 |
| GND | GND |

두 보드 모두 같은 방식으로 연결해야 한다.

## 4. 소프트웨어 구성

### `rf_transport`

CC1101과 직접 통신하는 계층이다.

- SPI 초기화 및 레지스터 설정
- GDO0 GPIO interrupt 등록
- ISR timestamp 큐
- CC1101 RX 상태 진입
- FIFO 패킷 읽기
- 채널 변경

### `fhss_slot_scheduler`

무선 장치를 모르는 순수 시간 계산 모듈이다.

- 기준 slot/time 저장
- 현재 슬롯 계산
- 특정 슬롯의 시작 시각 계산
- 다음 채널 변경 시각 계산
- overflow와 미동기 상태 검증

### `fhss_sync_controller`

SYNC 패킷과 timestamp를 하나로 묶는다.

```text
SYNC slot_number + GDO0 timestamp
                ↓
RX reference slot/time 설정
                ↓
예상 수신 시각과 실제 시각 비교
```

### `fhss_service`

실제 FreeRTOS 태스크와 RF 운용을 담당한다.

- TX SYNC 전송
- RX 채널 검색
- synchronized hopping
- timeout 처리
- 상위 FSM callback

### `fhss_fsm`

FHSS 내부 상태만 관리한다.

```text
SEARCHING
    ↓ 첫 SYNC
SYNCHRONIZING
    ↓ 연속 3회 성공
TRACKING
    ↓ 연속 5회 실패
SEARCHING
```

전역 `main/fsm`과 분리했기 때문에 무선 동기 상태를 독립적으로 디버깅할 수 있다.

## 5. 실제 동작

### TX

1. 슬롯 시작 5 ms 전에 다음 채널 선택
2. 슬롯 시작까지 대기
3. 현재 `slot_number`를 SYNC 패킷에 넣어 송신
4. GDO0 timestamp를 진단 로그로 기록
5. 다음 슬롯으로 이동

TX는 최초 시간 기준을 계속 유지한다. GDO0 timestamp로 매번 다시 기준을 잡으면 패킷 전송 지연이 슬롯마다 누적되므로 관측 용도로만 사용한다.

### RX 최초 동기화

1. 채널 0, 10, 20을 순환하며 SYNC 탐색
2. 첫 SYNC 수신
3. 패킷의 `slot_number`와 GDO0 timestamp 저장
4. 다음 슬롯 시작 시각과 채널 예측
5. 연속 3회 정상 수신
6. `SYNC_ACQUIRED`, 상태 `TRACKING`

### RX 동기 상실

1. 예상 슬롯에서 패킷 미수신
2. MISS 누적
3. 연속 5회 MISS
4. `SYNC_LOST`
5. 기존 시간 기준 제거
6. `SEARCHING`으로 돌아가 채널 검색

## 6. Main FSM 연결 정책

`SYNC_ACQUIRED`는 현재 시스템 상태를 바꾸지 않는다. 동기화 성공을 기록하고 표시하기 위한 관측 이벤트다.

`SYNC_LOST`는 안전장치 이벤트다. 통화나 수신 중 호핑 추종을 완전히 놓치면 전역 FSM을 `MENU_IDLE`로 돌려 정해진 시작 채널에서 다시 기다리게 한다.

```text
FHSS ACQUIRED → 로그/상태 표시
FHSS LOST     → MENU_IDLE 안전 복귀
```

## 7. 실제 테스트 결과

### 환경

- COM3: TX
- COM5: RX
- 슬롯: 300 ms
- 채널: 0, 10, 20
- 채널 변경 guard: 5 ms
- 획득: 연속 3회
- 상실: 연속 5회

### TX 시간 정확도

```text
slot=71 timestamp=21457625
slot=72 timestamp=21757627  delta=300002 us
slot=73 timestamp=22057626  delta=299999 us
```

설정한 300 ms 슬롯을 약 ±2 us 오차로 유지했다.

### RX 추종 결과

```text
TRACKING slot=307 channel=10 error=-2 us
TRACKING slot=308 channel=20 error=1 us
TRACKING slot=309 channel=0  error=-3 us
```

- TX/RX slot 일치
- TX/RX channel 일치
- 채널 0, 10, 20 반복 추종
- 일반적인 timing error 약 `-3 ~ +1 us`

### 강제 상실/재획득

TX를 약 3초 reset했다.

```text
SYNC_LOST
SEARCHING slot=5 channel=20
SYNCHRONIZING slot=6 channel=0
SYNCHRONIZING slot=7 channel=10
SYNC_ACQUIRED
TRACKING slot=8 channel=20
```

동기 상실, 검색 복귀, TX 재부팅 후 자동 재획득까지 확인했다.

## 8. 트러블슈팅

### GDO0 interrupt가 발생하지 않을 때

확인 순서:

1. CC1101 GDO0와 ESP32 GPIO18 연결 확인
2. GND 공통 연결 확인
3. `IOCFG0=0x06` 설정 확인
4. 채널 변경 후 `rf_transport_start_receive()`가 호출되는지 확인
5. GPIO interrupt가 상승 에지로 설정됐는지 확인

GDO0를 기다리기 전에 CC1101이 RX 상태에 들어가 있어야 한다. IDLE 상태에서는 sync word를 검출할 수 없다.

### FreeRTOS queue assert와 반복 리셋

발생했던 로그:

```text
assert failed: xQueueGenericSendFromISR
```

원인은 ISR 등록 대상 객체의 주소가 초기화 후 변경된 것이었다. 임시 `rf_transport` 객체 주소로 ISR을 등록한 뒤 최종 서비스 객체로 복사하면 ISR이 사라진 스택 주소를 참조한다.

해결:

- 최종 서비스 객체 안의 `rf_transport`를 직접 초기화
- ISR 등록 후 객체를 복사하거나 이동하지 않음

### 슬롯이 300 ms보다 계속 길어질 때

처음에는 timestamp 간격이 약 302.7 ms였다.

```text
설정 슬롯 300 ms
+ sync word 송신 지연 약 2.7 ms
= 실제 다음 기준 302.7 ms
```

TX가 매 GDO0 timestamp로 시간 기준을 다시 잡아 지연이 누적된 것이 원인이었다. TX는 고정 기준을 유지하고 GDO0 timestamp는 측정에만 사용하도록 수정했다.

### RX가 첫 패킷 후 다음 패킷을 못 받을 때

- 수신한 `slot_number`가 scheduler 기준으로 저장됐는지 확인
- 다음 슬롯 채널을 5 ms 전에 선택하는지 확인
- payload의 hop index와 계산된 hop index 비교
- timing error가 early/late window 안인지 확인
- SYNCHRONIZING 중 MISS가 발생하면 SEARCHING으로 돌아가는지 확인

### `ptt_button` 또는 `rotary_encoder` watchdog

발생했던 로그:

```text
task_wdt: IDLE0 did not reset
CPU 0: ptt_button
CPU 1: rotary_encoder
```

2 ms 또는 5 ms가 현재 FreeRTOS tick에서 0 tick으로 변환되면서 polling 태스크가 CPU를 계속 점유했다.

해결:

```c
const TickType_t ticks = pdMS_TO_TICKS(POLL_MS);
vTaskDelay(ticks > 0U ? ticks : 1U);
```

전역 FSM 활성 상태에서 watchdog 재발이 없음을 확인했다.

### 특정 방향의 Timing Window가 설정과 다를 때

기존 코드가 early 경계에도 `late_margin_us`를 사용하고 있었다. `early_margin_us`를 사용하도록 수정했다. early/late 값을 다르게 설정할 때 특히 확인해야 한다.

## 9. 로그 판정 방법

정상 동기 획득:

```text
SEARCHING
SYNCHRONIZING
SYNC_ACQUIRED
TRACKING
```

정상 추종:

```text
slot이 1씩 증가
channel이 0 → 10 → 20 반복
timing error가 허용 범위 안에서 유지
```

정상 상실 처리:

```text
SYNC_LOST
SEARCHING
```

주의해야 할 로그:

- timestamp가 항상 0: GDO0 배선 또는 interrupt 문제
- slot이 건너뜀: 태스크 지연 또는 처리 시간이 슬롯보다 김
- channel과 packet hop 불일치: 홉 테이블 또는 slot 기준 불일치
- timing error가 한 방향으로 계속 증가: TX/RX clock drift 또는 잘못된 재기준
- assert 후 반복 부팅: ISR에서 잘못된 객체/큐 주소 접근 가능성

## 10. 현재 상태와 다음 작업

현재 완료:

- GDO0 timestamp
- slot scheduler
- SYNC 기반 기준 설정
- 3채널 synchronized hopping
- 상실 및 자동 재획득
- main FSM 이벤트 연결
- 실기기 검증 및 원격 브랜치 push

다음 작업 후보:

1. 장시간 clock drift 측정
2. 채널별 성공률과 timeout 통계
3. `sync_offset_us` 실측 보정
4. slot number wrap-around 처리
5. 오디오 프레임의 슬롯 배치
6. OTA 모드 진입 시 FHSS 추적 정지/재개 정책

## 11. Git 정보

- 기준 브랜치: `feature/rf-transport-cc1101`
- 작업 브랜치: `feature/fhss-radio-sync-integration`
- 커밋:
  - `8695cae`
  - `3ee35f9`
  - `028c968`
- Pull Request: [#24 GDO0-based synchronized FHSS hopping](https://github.com/fhss-ota-radio/firmware-esp32/pull/24) (Draft)
