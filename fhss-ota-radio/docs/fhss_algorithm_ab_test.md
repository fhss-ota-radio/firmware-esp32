# FHSS 알고리즘 A/B 장시간 테스트

## 목적

동일한 무선 환경에서 시간 보정 전·후 펌웨어를 각각 2시간 운용해 동기
유지율, 제한 복구 성공률, 완전 재탐색 횟수와 보정 안정성을 비교한다.

## 비교 대상

| 구분 | 기준 커밋 | 설명 |
|---|---|---|
| A | b3a6f78 | seeded hopping과 N/N-1/N+1 복구, 시간 보정 없음 |
| B | 8086096 이후 | adaptive phase correction 적용 |

두 펌웨어는 같은 보드, 거리, 안테나 방향, 전원과 주변 RF 환경에서 측정한다.
한 번의 실험에서는 알고리즘 파라미터를 하나만 변경한다.

## 수집 로그

fhss_service는 diagnostics_interval_ms마다 누적 통계를 출력한다.

~~~text
FHSS_CSV_HEADER,uptime_ms,state,valid,...,correction_max_abs_us
FHSS_CSV,5000,TRACKING,16,0,2,1,0,-42,-3,35,0,0,0,1,0,0,8,12,31
~~~

| 열 | 의미 |
|---|---|
| valid | 정상 SYNC 수신 누계 |
| crc_fail | CRC 실패 누계 |
| timeout | GDO timestamp timeout 누계 |
| acquired, lost | 동기 획득·상실 이벤트 누계 |
| timing_min/avg/max_us | 보정 전 측정한 수신 timing error |
| recovery_entry | 제한 복구 진입 횟수 |
| recovery_success | 제한 복구 성공 횟수 |
| hard_research | 제한 복구 실패 후 완전 재탐색 횟수 |
| max_misses | 관측된 최대 연속 SYNC MISS |
| recovery_avg/max_us | 제한 복구 소요 시간 |
| correction_applied | 0이 아닌 시간 보정 적용 횟수 |
| correction_avg/max_abs_us | 적용 보정량 절댓값의 평균·최댓값 |

## 로그 저장

보드 포트에 맞춰 monitor 출력을 파일로 저장한다.

~~~powershell
idf.py -p COM8 monitor | Tee-Object fhss_b_2h.log
~~~

실험 종료 후 CSV 행만 추출한다.

~~~powershell
Select-String -Path fhss_b_2h.log -Pattern 'FHSS_CSV(_HEADER)?,' |
    ForEach-Object { $_.Line -replace '^.*FHSS_CSV', 'FHSS_CSV' } |
    Set-Content fhss_b_2h.csv
~~~

## 핵심 판정식

~~~text
복구 성공률 = recovery_success / recovery_entry
완전 재탐색률 = hard_research / 전체 관측 슬롯
SYNC 수신 성공 비율 = valid / (valid + timeout + crc_fail)
~~~

시간 보정 적용 후에는 다음 조건을 함께 만족해야 개선으로 판단한다.

1. hard_research와 lost가 감소한다.
2. 복구 성공률이 증가하거나 유지된다.
3. timing error가 한 방향으로 계속 증가하지 않는다.
4. correction_max_abs_us가 반복적으로 슬롯 길이에 근접하지 않는다.
5. CRC 실패와 timeout이 증가하지 않는다.

## 테스트 순서

1. CONFIG_FREERTOS_HZ=1000, 보드 Flash 종류와 submodule 상태를 확인한다.
2. A 펌웨어를 양쪽 보드에 올리고 2시간 로그를 저장한다.
3. 같은 배치에서 B 펌웨어로 바꾸고 2시간 로그를 저장한다.
4. 정상 환경 비교 후 TX를 1~4 슬롯 동안 차단해 제한 복구를 검증한다.
5. TX를 loss 임계값 이상 차단해 완전 재탐색과 재획득을 검증한다.
6. 결과를 바탕으로 deadband, 보정 divisor 또는 복구 임계값 중 하나만 조정한다.

현재 코드는 계측 및 CSV 출력까지 구현됐다. 보드가 연결되지 않은 상태에서는
ESP32-S3 개별 소스 컴파일까지만 검증했으며, 실제 2시간 결과는 아직 없다.
