# FHSS Sync State 기능 구현

> 연속 유효 수신과 연속 미수신 횟수를 추적하여 FHSS 동기 획득 및 상실을 판정하는 작은 상태 머신이다.

## 1. 기능 개요

- 목적: 일시적인 패킷 손실에 즉시 동기를 해제하지 않고 설정된 임계값을 기준으로 안정적으로 상태를 전환한다.
- 상태: `SEARCHING`, `LOCKED`
- 이벤트: `NONE`, `ACQUIRED`, `LOST`
- 동적 메모리 및 운영체제 의존성: 없음

## 2. 상태 머신

| 현재 상태 | 입력 | 처리 | 다음 상태 | 이벤트 |
|---|---|---|---|---|
| SEARCHING | VALID | 연속 성공 증가 | 임계값 미만이면 SEARCHING | NONE |
| SEARCHING | VALID | 획득 임계값 도달 | LOCKED | ACQUIRED |
| SEARCHING | MISS | 연속 성공 초기화 | SEARCHING | NONE |
| LOCKED | VALID | 연속 MISS 초기화 | LOCKED | NONE |
| LOCKED | MISS | 연속 MISS 증가 | 임계값 미만이면 LOCKED | NONE |
| LOCKED | MISS | 상실 임계값 도달 | SEARCHING | LOST |

## 3. 설정과 내부 상태

```c
typedef struct {
    uint32_t acquire_count;
    uint32_t loss_count;
} fhss_sync_state_config_t;

typedef struct {
    fhss_sync_state_t state;
    uint32_t consecutive_valid;
    uint32_t consecutive_misses;
    fhss_sync_state_config_t config;
    uint8_t initialized;
} fhss_sync_state_tracker_t;
```

- `acquire_count`: SEARCHING에서 LOCKED로 전환하기 위한 연속 정상 수신 횟수
- `loss_count`: LOCKED에서 SEARCHING으로 전환하기 위한 연속 미수신 횟수

## 4. 공개 API

| 함수 | 역할 |
|---|---|
| `fhss_sync_state_init()` | 설정 검증 및 SEARCHING 상태 초기화 |
| `fhss_sync_state_on_valid()` | 정상 수신 반영 및 획득 이벤트 판정 |
| `fhss_sync_state_on_miss()` | 미수신 반영 및 상실 이벤트 판정 |
| `fhss_sync_state_get()` | 현재 상태 조회 |

## 5. 오류 처리

| 상태 | 조건 |
|---|---|
| `FHSS_SYNC_STATUS_INVALID_ARG` | 필수 포인터가 `NULL` |
| `FHSS_SYNC_STATUS_INVALID_CONFIG` | 획득 또는 상실 임계값이 0 |
| `FHSS_SYNC_STATUS_NOT_INITIALIZED` | 초기화 전에 이벤트 처리 또는 상태 조회 |

## 6. 상위 계층 연동

```text
타이밍 윈도우 안의 정상 패킷 → on_valid()
타이밍 윈도우 밖의 패킷     → on_miss()
예상 시각까지 미수신         → on_miss()
                              ↓
                    ACQUIRED / LOST 이벤트
```

`LOST` 이벤트는 최상위 FSM이 `MENU_IDLE`로 복귀하는 안전장치의 입력 후보다. 정상 수신마다 FSM 이벤트를 발생시키지 않고 상태 전이 순간만 통지한다.

## 7. 검증 결과

| 항목 | 결과 |
|---|---|
| 전체 ESP-IDF 빌드 | 성공 |
| 애플리케이션 바이너리 생성 | 성공 |
| 상태 전이 단위 테스트 | 후속 작업 |
| 실제 무선 패킷 손실 시험 | 미검증 |

## 8. 후속 작업

1. 임계값 1과 다회 임계값에 대한 상태 전이 단위 테스트 추가
2. 카운터 오버플로 방지를 위한 포화 증가 적용 여부 검토
3. OTA 모드 진입 시 추적 중지 또는 초기화 정책 확정
4. `FHSS_SYNC_EVENT_LOST`를 상위 FSM 이벤트로 변환하는 연결부 구현

## 9. Git 정보

- 브랜치: `fix/fhss-sync-state-implementation`
- 기준 브랜치: `fix/fhss-hop-sequence-implementation`
- 커밋: `5fb535f`
- 후속 스택: `feature/fhss-core-integration`

