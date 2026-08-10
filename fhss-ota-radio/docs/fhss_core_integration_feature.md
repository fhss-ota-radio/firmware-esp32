# FHSS Core 통합 기능

> FHSS 패킷 검증, 수신 타이밍 판정, 동기 상태 추적, 홉 채널 계산을 단일 인터페이스로 제공하는 조정 계층이다.

## 1. 기능 개요

- 입력: raw SYNC 패킷, 패킷 길이, 예상 수신 시각, 실제 수신 시각
- 출력: 디코딩된 패킷, 타이밍 판정, 동기 이벤트·상태, 홉 인덱스·채널
- 담당 모듈: `components/fhss_core/fhss_core.*`
- 하드웨어 및 RTOS 의존성: 없음

## 2. 구성 모듈

| 모듈 | 책임 |
|---|---|
| `fhss_sync_packet` | SYNC 패킷 형식 검증과 decode |
| `fhss_timing_window` | 예상 시각 대비 실제 수신 시각 분류 |
| `fhss_sync_state` | 연속 VALID/MISS 기반 동기 상태 추적 |
| `fhss_hop_sequence` | 슬롯 번호 기반 홉 인덱스·채널 계산 |
| `fhss_core` | 위 모듈의 호출 순서와 오류 매핑 조정 |

## 3. 초기화

`fhss_core_init()`은 임시 구조체에 하위 모듈을 먼저 초기화하고 모두 성공한 뒤 호출자의 `core`에 복사한다. 따라서 중간 실패가 발생해도 부분 초기화된 상태를 노출하지 않는다.

필수 설정은 다음과 같다.

- 채널 배열과 채널 수
- Timing Window의 early/late margin
- Sync State의 획득/상실 임계값

## 4. 수신 처리

`fhss_core_process_rx()`의 처리 순서는 다음과 같다.

1. 포인터와 Core 초기화 상태 확인
2. SYNC 패킷 decode 및 프로토콜 필드 검증
3. 예상·실제 수신 시각으로 Timing Window 평가
4. 윈도우 내부면 VALID, 외부면 MISS로 Sync State에 반영
5. 현재 동기 상태 조회
6. 패킷의 `slot_number`로 홉 인덱스와 채널 계산
7. 모든 단계가 성공한 후 결과 구조체 반환

처리 도중 실패하면 호출자의 `out_result`는 수정하지 않는다.

## 5. 타임아웃 및 채널 조회

### 타임아웃

`fhss_core_handle_timeout()`은 예상 수신 시각까지 패킷이 없을 때 MISS를 한 번 반영하고, 발생 이벤트와 현재 상태를 반환한다.

### 채널 조회

`fhss_core_get_channel()`은 Core 초기화 상태를 확인한 뒤 주어진 슬롯에 해당하는 채널을 반환한다.

## 6. 공개 결과 구조체

```c
typedef struct {
    fhss_sync_packet_t packet;
    fhss_timing_window_evaluation_t timing;
    fhss_sync_event_t sync_event;
    fhss_sync_state_t sync_state;
    uint8_t hop_index;
    uint8_t channel;
} fhss_core_rx_result_t;
```

상위 계층은 이 구조체를 사용해 별도의 하위 모듈 호출 없이 현재 수신의 전체 판단 결과를 얻을 수 있다.

## 7. 시스템 연동 위치

```text
CC1101 IRQ / rf_transport
        ↓ raw packet + timestamp
fhss_core_process_rx()
        ↓ channel / sync event / packet info
RF 채널 전환 및 상위 FSM
```

- `rf_transport`가 예상·실제 수신 시각을 제공해야 한다.
- `LOST` 이벤트는 상위 FSM의 `FSM_EVENT_SYNC_LOST`로 변환할 수 있다.
- OTA 수신 중에는 음성 FHSS MISS 카운팅을 중지하는 정책이 별도로 필요하다.

## 8. 빌드 및 검증

| 항목 | 결과 |
|---|---|
| ESP-IDF 전체 빌드 | 성공 |
| `fhss-ota-radio.bin` 생성 | 성공 |
| 바이너리 크기 | `0x46e00` |
| 최소 앱 파티션 여유 | 72% |
| 실제 CC1101 연동 | 미검증 |
| 통합 단위 테스트 | 후속 작업 |

빌드 명령:

```powershell
ninja -C build
```

## 9. 알려진 제약과 후속 작업

1. Timing Window 구현에서 `early_margin_us`가 실제 경계 계산에 반영되는지 별도 검토가 필요하다.
2. `expected_rx_time_us` 생성과 슬롯 스케줄러는 아직 Core 외부 책임이다.
3. `rf_transport`와 CC1101 드라이버 연결이 필요하다.
4. 정상·오류·경계값을 포함한 통합 단위 테스트가 필요하다.
5. OTA 진입과 종료 시 Sync State 유지·초기화 정책을 확정해야 한다.

## 10. Git 정보

- 브랜치: `feature/fhss-core-integration`
- 기준 브랜치: `fix/fhss-sync-state-implementation`
- 커밋: `feec51e`
- 전체 스택 기준 브랜치: `develop`
