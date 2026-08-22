# FHSS 시간 보정 알고리즘 A/B 테스트

## 목적

단일 수신 지연을 실제 clock drift로 오판해 슬롯 기준을 과도하게 이동시키는 문제를 재현하고, 1회 보정 상한 적용 전후의 동기 유지 성능을 비교한다.

## 공통 조건

| 항목 | 값 |
|---|---:|
| 보드 | ESP32-S3 2대, CC1101 2대 |
| TX / RX | COM6 / COM10 |
| 슬롯 길이 | 300,000 us |
| Timing Window | ±20,000 us |
| 강제 지연 | +19,000 us |
| 주입 주기 | TRACKING SYNC 10회마다 |
| PTT 유지 | 약 15초 |

강제 지연은 RX가 controller에 전달하는 timestamp에만 더했다. 실제 RF airtime과 채널 환경은 바꾸지 않았으므로 시간 보정 알고리즘만 비교할 수 있다.

20,000 us를 그대로 주입하면 실제 지터가 더해져 Timing Window 바깥으로 분류될 수 있다. 실제 장애에서 관측한 약 19.7 ms 지연을 윈도우 내부에서 재현하기 위해 19,000 us를 사용했다.

## 비교 설정

| 구분 | 1회 최대 보정량 | 의미 |
|---|---:|---|
| A 기준 | 30,000 us | 사실상 상한 없음 |
| B 후보 | 500 us | 단일 이상치의 영향 제한 |

- ±500 us: deadband, 보정 없음
- 500~2,000 us: 초과분의 1/8 보정
- 2,000 us 초과: 추가 초과분의 1/2 보정
- B에서는 최종 계산 결과를 최대 500 us로 제한

## 실제 결과

| 지표 | A 기준 | B 후보 | 변화 |
|---|---:|---:|---:|
| 정상 수신 SYNC(초기 단일 실행) | 13 | 54 | +41, 4.15배 |
| 정상 추종 증가율(초기 단일 실행) | 기준 | 기준 대비 | +315% |
| 반복 강제 이상치 통과 | 0/10회 | 16/16회 | 생존율 0% → 100% |
| 최대 1회 보정 | 8,691 us | 500 us | 94.2% 감소 |
| 강제 이상치 후 SYNC_LOST | 10/10회 | 0/16회 | 실패율 100% → 0% |
| B 연속 TRACKING | 해당 없음 | 180슬롯 이상 | 54초 이상 |

### A 기준 로그

```text
A/B FAULT: sync_sample=20 injected_delay=19000 us
SYNC RX: slot=13 error=19009 us correction=8691 us
RECOVERY entered after 2 consecutive sync misses
SYNC_LOST
```

첫 이상치를 실제 drift로 오판해 슬롯 기준을 8.691 ms 이동했다. 이후 SYNC를 한 개도 추가로 받지 못하고 전체 채널 검색으로 복귀했다.

### B 후보 로그

```text
A/B FAULT: sync_sample=10 injected_delay=19000 us
SYNC RX: slot=12 error=19032 us correction=500 us
SYNC RX: slot=14 error=-977 us correction=-59 us

A/B FAULT: sync_sample=20 injected_delay=19000 us
SYNC RX: slot=22 error=18343 us correction=500 us
```

B는 19 ms 이상치의 영향을 500 us로 제한했다. 이후 정상 패킷에서 약 -16~-96 us씩 완만하게 반대 방향으로 보정하며 TRACKING을 유지했다.

반복 시험에서 A는 유효 강제 이상치 10회가 모두 SYNC_LOST로 이어졌다. B는 목표 10회를 넘어 동일 세션에서 최소 16회의 강제 이상치를 연속 통과했고, 약 180슬롯(54초) 이상 TRACKING을 유지했다. 따라서 이 시험 조건의 이상치 생존율은 0%에서 100%로, 동기 상실률은 100%에서 0%로 개선됐다.

PTT 해제 뒤 발생한 recovery와 SYNC_LOST는 송신 종료 패킷이 아직 없어 정상 세션 종료를 미수신으로 해석한 별도 문제이며, 시간 보정 실패 통계에서는 제외했다.

## 결론

500 us 상한은 단일 스케줄링 지연이 전체 슬롯 기준을 훼손하는 것을 막았다. 초기 단일 실행에서 정상 추종 길이는 13개에서 54개로 315% 증가했다. 반복 시험에서는 A가 10/10회 실패한 반면 B는 최소 16/16회 성공했다.

이 결과는 의도적으로 주입한 19 ms 단발 지연에 대한 값이다. 실제 RF 간섭, 장기 oscillator drift 및 정상 세션 종료 문제는 별도 2시간 시험으로 검증해야 한다.

## 다음 실험

1. 지연 5/10/15/19 ms 단계별 sweep
2. 500/1,000/2,000 us 보정 상한 비교
3. 정상 환경에서 불필요한 보정과 음성 패킷 손실률 비교
4. 2시간 장기 시험으로 clock drift와 채널별 성공률 측정

## 정상 펌웨어 설정

실험 후 timestamp fault injection은 비활성화하고 보정 상한 500 us를 유지한다.

```c
#define FHSS_SERVICE_TEST_DELAY_ENABLED 0U
#define FHSS_SERVICE_TEST_DELAY_PERIOD  10U
#define FHSS_SERVICE_TEST_DELAY_US      19000LL
```
