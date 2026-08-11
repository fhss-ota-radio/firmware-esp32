# FHSS 동기화 진단 통계 및 Clock Drift 측정

> synchronized hopping 상태를 숫자로 관찰해 RF 손실, 채널 편향, 시간 오차를 구분하기 위한 진단 기능이다.

## 1. 목적

기존 로그는 각 SYNC 패킷의 슬롯과 timing error를 보여줬지만, 장시간 동작의 추세와 채널별 품질을 한눈에 비교하기 어려웠다. 이번 기능은 다음 값을 누적하고 기본 5초마다 요약한다.

- 정상 SYNC 수신 수
- CRC 실패 수
- 수신 timeout 수
- 동기 획득 및 상실 횟수
- timing error 최소·평균·최대
- 마지막 정상 SYNC 이후 경과 시간
- CC1101 채널별 정상 수신·CRC 실패·timeout 수

## 2. 구현 위치

| 파일 | 책임 |
|---|---|
| `components/fhss_service/fhss_diagnostics.c` | 하드웨어 독립적인 통계 누적 |
| `components/fhss_service/include/fhss_diagnostics.h` | 통계 및 snapshot 공개 형식 |
| `components/fhss_service/fhss_service.c` | 수신 결과 분류, 주기 로그, thread-safe snapshot |
| `main/main.c` | `diagnostics_interval_ms` 설정 |

`fhss_service_get_diagnostics()`를 사용하면 로그 외의 UI나 상위 진단 계층에서도 같은 snapshot을 읽을 수 있다. 서비스 태스크와 외부 호출이 동시에 접근할 수 있으므로 복사 과정은 FreeRTOS mutex로 보호한다.

## 3. 수신 결과 분류

기존의 단일 성공/실패 결과를 다음과 같이 분리했다.

| 결과 | 통계 | Sync State의 MISS |
|---|---|---|
| 정상 SYNC | `rx_valid_count` 증가 | 아니오 |
| GDO0 timeout | `timeout_count` 증가 | 예 |
| CRC 실패 | `crc_fail_count` 증가 | 예 |
| SPI/FIFO 오류 | 경고 및 service error | 아니오 |
| SYNC decode/controller 오류 | 경고 및 service error | 아니오 |

SPI나 내부 처리 오류를 RF timeout으로 집계하지 않기 때문에 통신 품질과 소프트웨어 오류를 분리할 수 있다.

## 4. 로그 형식과 해석

전체 요약:

```text
DIAG state=TRACKING valid=118 crc_fail=0 timeout=4 acquired=1 lost=0 timing_us[min/avg/max]=-6/-1/5 last_valid_age_ms=9
```

- `state`: 현재 FHSS FSM 상태
- `valid`: 시작 이후 정상 해석한 SYNC 누계
- `crc_fail`: CC1101이 CRC 오류로 판정한 패킷 누계
- `timeout`: GDO0를 제한 시간 안에 받지 못한 누계
- `acquired`, `lost`: 동기 획득·상실 전이 횟수
- `timing_us[min/avg/max]`: 정상 SYNC timing error의 누적 범위와 평균
- `last_valid_age_ms`: 마지막 정상 SYNC 이후 경과 시간, 아직 받은 적이 없으면 `-1`

채널별 요약:

```text
DIAG channel=0  valid=39 crc_fail=0 timeout=1
DIAG channel=10 valid=39 crc_fail=0 timeout=2
DIAG channel=20 valid=40 crc_fail=0 timeout=1
```

초기 SEARCHING 중에는 TX와 다른 채널을 듣기 때문에 timeout이 발생할 수 있다. 따라서 누적 timeout 값 하나보다 `TRACKING` 진입 후 값이 계속 증가하는지를 봐야 한다.

## 5. 실기기 Smoke Test 결과

### 환경

- COM3: 기존 synchronized FHSS TX
- COM5: 진단 기능이 적용된 RX
- 슬롯: 300 ms
- 채널: `CHANNR {0, 10, 20}`
- 통계 출력: 5초 간격
- 관찰 시간: 약 20초

### 결과

```text
DIAG state=TRACKING valid=67  crc_fail=0 timeout=4 acquired=1 lost=0 timing_us[min/avg/max]=-6/-1/5
DIAG state=TRACKING valid=84  crc_fail=0 timeout=4 acquired=1 lost=0 timing_us[min/avg/max]=-6/-1/5
DIAG state=TRACKING valid=101 crc_fail=0 timeout=4 acquired=1 lost=0 timing_us[min/avg/max]=-6/-1/5
DIAG state=TRACKING valid=118 crc_fail=0 timeout=4 acquired=1 lost=0 timing_us[min/avg/max]=-6/-1/5
```

| 검증 항목 | 결과 |
|---|---|
| 5초 주기 출력 | 성공 |
| TRACKING 유지 | 성공 |
| CRC 실패 | 0회 |
| 동기 상실 | 0회 |
| 채널별 수신 균형 | 39 / 39 / 40 |
| timing error | 최소 -6 µs, 평균 -1 µs, 최대 +5 µs |
| ESP-IDF 빌드 | 성공 |
| 펌웨어 크기 | `0x4fd90`, 앱 파티션 69% 여유 |

## 6. 장시간 Clock Drift 측정 방법

1. COM3 TX를 먼저 실행하고 COM5 RX를 시작한다.
2. RX가 `TRACKING`이고 `acquired=1`인지 확인한다.
3. 10분, 30분, 1시간 시점의 `DIAG` 줄을 저장한다.
4. 평균값과 최소·최대 범위가 시간에 따라 한 방향으로 이동하는지 비교한다.
5. `lost` 증가 시 해당 직전의 `last_valid_age_ms`, 채널별 timeout, CRC 실패를 함께 확인한다.

판단 예시:

- 평균 오차가 0 근처에서 유지: clock drift가 현재 guard 범위에서 안정적
- 평균과 최대·최소가 같은 방향으로 계속 이동: TX/RX clock drift 가능성
- 특정 채널의 CRC만 증가: 해당 주파수 간섭 또는 RF 품질 문제 가능성
- 모든 채널의 timeout과 `last_valid_age_ms`가 함께 증가: TX 정지, 전원 또는 전체 링크 문제 가능성
- `sync_offset_us`를 조정할 때는 짧은 구간의 단일 값이 아니라 충분히 긴 구간의 평균을 기준으로 한다.

## 7. 설정

`main/main.c`:

```c
.diagnostics_interval_ms = 5000U,
```

- `0`: 주기 로그 비활성화, 내부 통계와 snapshot은 계속 유지
- `5000`: 5초마다 출력
- 장시간 측정 시 로그 양을 줄이려면 `60000`으로 설정해 1분 간격으로 확인

## 8. 다음 검증

- 1시간 이상 연속 동작에서 drift 추세 수집
- TX 강제 중단 시 timeout 및 `lost` 증가 확인
- 특정 채널 차폐 또는 거리 증가 시 채널별 CRC/timeout 분리 확인
- 누적 통계 reset API가 필요한지 상위 UI 요구사항과 함께 결정

## 9. Git 정보

- 기준 브랜치: `feature/fhss-radio-sync-integration`
- 작업 브랜치: `feature/fhss-sync-diagnostics`
- Pull Request: 아직 생성하지 않음

