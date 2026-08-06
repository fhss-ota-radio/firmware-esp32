# FHSS 타이밍 윈도우 평가

> 수신된 FHSS 슬롯 패킷의 수신 시각이 예상 수신 시각 대비 허용 범위 안에 있는지 판단하는 기능이다. 설정된 마진을 기준으로 `before`, `inside`, `after` 결과를 반환한다.

## 1. 기능 개요

- 목적: FHSS 동기화 수신 시점의 허용 범위를 판단하여 `SYNC_ACQUIRED` / `SYNC_LOST` 판정에 필요한 타이밍 정보를 제공
- 담당 모듈: `components/fhss_core`
- 구현 결과: `fhss_timing_window` 모듈 추가, 타이밍 오차 계산 및 윈도우 판정 함수 구현
- 현재 상태: 부분 완료 / 검증 중

## 2. 전체 구조에서의 위치

### 역할

- 이 기능은 `fhss_core` 내부에서 수신 타이밍 평가를 담당한다.
- `rf_transport`는 CC1101 수신 데이터와 타임스탬프를 전달하고, `fhss_core`는 동기화 패킷 검증과 타이밍 판정을 수행한다.
- `main/fsm`은 `fhss_core`의 평가 결과를 사용하여 동기 상태 전이를 처리한다.

### 데이터 흐름

```text
rf_transport
→ 예상 수신 시각(expected_rx_time_us), 실제 수신 시각(actual_rx_time_us)
→ fhss_timing_window
→ 타이밍 오차 계산 및 윈도우 판정
→ out_evaluation { timing_error_us, result }
→ 상위 FSM
```

- `expected_rx_time_us`: FHSS 슬롯과 호핑 시퀀스 계산 후 도출된 예상 도착 시각
- `actual_rx_time_us`: CC1101 수신 이벤트 시점
- `out_evaluation`: 타이밍 오류 및 판정 결과

## 3. 동작 방식

1. 입력 검증
   - `config` 또는 `out_evaluation`이 `NULL`이면 `FHSS_TIMING_STATUS_INVALID_ARG` 반환
2. 타이밍 오류 계산
   - `timing_error_us = actual_rx_time_us - expected_rx_time_us`
3. 허용 경계 계산
   - 현재 구현은 `late_margin_us`를 기준으로 앞/뒤 경계를 동일하게 계산
4. 조건 판정
   - `timing_error_us < -late_margin_us` → `FHSS_TIMING_BEFORE_WINDOW`
   - `timing_error_us > late_margin_us` → `FHSS_TIMING_AFTER_WINDOW`
   - 그 외 → `FHSS_TIMING_INSIDE_WINDOW`
5. 결과 반환
   - `out_evaluation`에 계산 결과 복사 후 `FHSS_TIMING_STATUS_OK` 반환

## 4. 핵심 설계

- 선택한 알고리즘: 고정 오프셋 기반 비교
- 이유: FHSS 수신 타이밍 판정은 단일 시점의 오차를 기준으로 간단하게 분류하는 것이 충분
- 입력과 출력
  - 입력: `fhss_timing_window_config_t`, `expected_rx_time_us`, `actual_rx_time_us`
  - 출력: `fhss_timing_window_evaluation_t`
- 주요 예외 처리
  - `NULL` 포인터 입력 시 오류 반환
- 실시간성/메모리 고려
  - 계산은 O(1), 정수 연산만 사용하여 임베디드 실시간 처리에 적합

## 5. 변경 파일

| 파일 경로 | 구분 | 역할 |
|---|---|---|
| `components/fhss_core/fhss_timing_window.c` | 생성 | 타이밍 윈도우 평가 로직 구현 |
| `components/fhss_core/include/fhss_timing_window.h` | 생성 | 평가 상태, 결과, config 및 공개 인터페이스 정의 |

## 6. 주요 인터페이스

```c
typedef enum {
    FHSS_TIMING_STATUS_OK = 0,
    FHSS_TIMING_STATUS_INVALID_ARG,
} fhss_timing_status_t;

typedef enum {
    FHSS_TIMING_BEFORE_WINDOW = 0,
    FHSS_TIMING_INSIDE_WINDOW,
    FHSS_TIMING_AFTER_WINDOW,
} fhss_timing_window_result_t;

typedef struct {
    uint32_t early_margin_us;
    uint32_t late_margin_us;
} fhss_timing_window_config_t;

typedef struct {
    int64_t timing_error_us;
    fhss_timing_window_result_t result;
} fhss_timing_window_evaluation_t;

fhss_timing_status_t fhss_timing_window_evaluate(
    const fhss_timing_window_config_t *config,
    int64_t expected_rx_time_us,
    int64_t actual_rx_time_us,
    fhss_timing_window_evaluation_t *out_evaluation
);
```

- 입력: 타이밍 윈도우 설정, 예상/실제 수신 시각, 출력 버퍼
- 출력: `timing_error_us` 및 윈도우 판정 결과
- 반환값: 상태 코드 (`OK` 또는 `INVALID_ARG`)
- 실패 조건: `config == NULL` 또는 `out_evaluation == NULL`
- 호출 모듈: `fhss_core` 내부 동기화 처리 흐름
- 결과 사용 모듈: 상위 FSM 또는 동기 상태 판단 로직

## 7. 빌드 및 검증

### 빌드

```powershell
정보 미제공
```

### 검증 결과

| 항목 | 결과 | 비고 |
|---|---|---|
| 전체 프로젝트 빌드 | 미실행 | |
| 정상 입력 | 미실행 | |
| 경계값 | 미실행 | |
| 오류 입력 | 미실행 | |
| 실제 하드웨어 | 미실행 | |

## 8. 제한사항 및 후속 작업

### 현재 제한사항

- `early_margin_us` 필드는 헤더에 정의되어 있으나, 현재 구현은 `late_margin_us`만 사용하여 앞/뒤 허용 폭을 동일하게 처리함
- 현재는 소프트웨어 단위 검증 결과가 제공되지 않음
- 실제 ESP32/CC1101 연동 검증이 아직 없음

### 다음 작업

1. `fhss_hop_sequence`와 연동하여 `expected_rx_time_us` 계산 경로 확인
2. 단위 테스트 추가 및 경계값 검증
3. 실제 하드웨어 수신 타이밍 검증
4. `early_margin_us` 사용 여부와 `config` 의미 재확인

### 인수인계 시작 지점

- 먼저 확인할 파일: `components/fhss_core/fhss_timing_window.c`, `components/fhss_core/include/fhss_timing_window.h`
- 먼저 읽을 문서: FHSS 동기화 패킷 처리 및 타이밍 설계 문서(정보 미제공)
- 먼저 실행할 명령: `정보 미제공`
- 연동 전 확인할 인터페이스: `expected_rx_time_us`와 `actual_rx_time_us` 전달 경로, `fhss_timing_window_evaluation_t` 사용 위치

## 9. Git 정보

- 브랜치: `feature/fhss-timing-window`
- 기준 브랜치: 확인 필요 (`develop` 예상)
- 주요 커밋: 정보 미제공
- 관련 Issue: 정보 미제공
- 관련 PR: 정보 미제공
