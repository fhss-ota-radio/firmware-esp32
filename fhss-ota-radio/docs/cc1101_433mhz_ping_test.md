# CC1101 433 MHz 통신 및 3채널 FHSS Smoke Test

## 1. 목적

두 대의 ESP32-S3와 CC1101을 이용해 다음 항목을 단계적으로 확인한다.

1. ESP32-S3와 CC1101 사이의 SPI 통신
2. 433.92 MHz 고정 채널 패킷 송수신
3. CC1101 `CHANNR` 변경
4. `fhss_hop_sequence`가 계산한 채널을 실제 무선 장치에 적용
5. 채널 0, 10, 20에서 패킷 수신

이 테스트는 실제 FHSS 시간 동기화 이전의 하드웨어 검증이다. TX와 RX의 슬롯 경계를 맞추는 SYNC 기능은 포함하지 않는다.

## 2. 테스트 환경

| 항목 | 설정 |
|---|---|
| TX 보드 | COM3 |
| RX 보드 | COM5 |
| 모듈 | CC1101 433 MHz, 안테나 연결 |
| 중심 주파수 | 433.92 MHz |
| 변조 | 2-FSK |
| 데이터 속도 | 약 38.4 kBaud |
| 패킷 | 가변 길이, CRC 사용 |
| 송신 출력 | 약 -30 dBm |
| 홉 채널 | `CHANNR {0, 10, 20}` |
| TX 동작 | 채널당 100 ms 간격으로 3회 송신 |
| RX 검색 체류 시간 | 137 ms |

RX 체류 시간을 200 ms로 사용하면 TX의 채널당 300 ms 주기와 일정한 위상 관계가 생겨 채널 20을 계속 놓치는 현상이 있었다. 137 ms로 변경해 검색 시점이 특정 채널에 고정되지 않도록 했다.

## 3. 구현 위치

- `components/rf_transport/rf_transport.c`
  - SPI 초기화
  - CC1101 레지스터 설정
  - 패킷 송수신
  - `rf_transport_set_channel()`
- `components/fhss_core/fhss_hop_sequence.c`
  - 슬롯 번호를 홉 채널로 변환
- `main/main.c`
  - 현재 브랜치의 임시 하드웨어 테스트 하네스

`rf_transport_set_channel()`은 CC1101을 IDLE 상태로 전환하고 `CHANNR`를 기록한 뒤 RX/TX FIFO를 정리한다.

## 4. 실행 방법

TX 설정:

```c
#define CC1101_TEST_ROLE CC1101_TEST_ROLE_TX
#define CC1101_FHSS_TEST_ENABLED 1
```

COM3에 플래시한다.

```powershell
idf.py -p COM3 build flash
```

그다음 RX 설정으로 변경한다.

```c
#define CC1101_TEST_ROLE CC1101_TEST_ROLE_RX
```

COM5에 플래시하고 모니터를 실행한다.

```powershell
idf.py -p COM5 build flash monitor
```

현재 저장된 소스는 RX 설정이다.

## 5. 실제 검증 결과

2026-08-11에 COM3 TX와 COM5 RX를 사용해 세 채널 모두 연속 수신되는 것을 확인했다.

```text
FHSS RX PASS: scan_slot=156 channel=0  payload="FHSS:669:0:0"  RSSI=-54 dBm LQI=13
FHSS RX PASS: scan_slot=160 channel=10 payload="FHSS:670:10:1" RSSI=-56 dBm LQI=13
FHSS RX PASS: scan_slot=164 channel=20 payload="FHSS:671:20:2" RSSI=-59 dBm LQI=12
```

이후에도 채널 0, 10, 20 순서의 수신이 반복됐으며 출력은 모두 `FHSS RX PASS`였다.

| 검증 항목 | 결과 |
|---|---|
| COM3 TX 플래시 | 성공 |
| COM5 RX 플래시 | 성공 |
| CC1101 SPI 식별 정보 | `PARTNUM=0x00`, `VERSION=0x14` |
| 채널 0 수신 | 성공, 약 -54 dBm |
| 채널 10 수신 | 성공, 약 -56 dBm |
| 채널 20 수신 | 성공, 약 -59 dBm |
| CRC | 세 채널 모두 통과 |
| ESP-IDF 전체 빌드 | 성공 |
| 펌웨어 크기 | `0x33470`, 앱 파티션 80% 여유 |

## 6. 트러블슈팅과 디버깅

### 6.1 권장 디버깅 순서

FHSS 수신이 되지 않을 때 처음부터 홉 로직을 의심하면 원인 범위가 너무 넓어진다. 다음 순서대로 한 계층씩 확인한다.

```text
COM 포트 연결
    ↓
펌웨어 flash 및 부팅 로그
    ↓
CC1101 PARTNUM / VERSION
    ↓
고정 채널 PING
    ↓
채널별 단독 송수신
    ↓
3채널 순환 검색
    ↓
SYNC 기반 슬롯 동기화
```

앞 단계가 실패한 상태에서는 다음 단계의 결과를 신뢰할 수 없다. 예를 들어 `PARTNUM` 읽기가 실패하면 FHSS 계산이 정상이어도 실제 채널 변경 여부를 판단할 수 없다.

### 6.2 COM 포트가 열리지 않는 경우

대표 로그:

```text
Could not open COM5, the port is busy or doesn't exist.
PermissionError(13, '액세스가 거부되었습니다.')
```

먼저 현재 포트를 확인한다.

```powershell
[System.IO.Ports.SerialPort]::GetPortNames()
```

포트가 목록에 있는데도 열리지 않으면 기존 ESP-IDF monitor가 포트를 점유하고 있을 가능성이 높다.

```powershell
Get-CimInstance Win32_Process |
    Where-Object { $_.CommandLine -match 'COM5|idf_monitor' } |
    Select-Object ProcessId, Name, CommandLine
```

출력된 명령줄이 실제 COM5 monitor인지 확인한 뒤 해당 PID만 종료한다. 예전 PID를 한꺼번에 종료하면 이미 끝난 프로세스에 대해 `NoProcessFoundForGivenId`가 출력될 수 있지만, 이는 보드나 펌웨어 오류가 아니다.

### 6.3 Flash와 Monitor 문제를 구분하는 방법

`build`, `flash`, `monitor`는 서로 다른 단계다.

- `build` 성공: 소스와 컴포넌트 연결이 컴파일됨
- `flash` 성공: 바이너리가 ESP32-S3 플래시에 기록됨
- `monitor` 성공: PC가 UART/USB 로그 포트를 열었음

따라서 빌드가 성공했어도 COM 포트 점유 때문에 flash가 실패할 수 있고, flash가 성공했어도 monitor만 실패할 수 있다. 로그의 마지막 `Executing action`과 `A fatal error occurred` 위치를 확인해 실패 단계를 먼저 구분한다.

### 6.4 SPI 연결 진단

정상 로그:

```text
CC1101 SPI OK: PARTNUM=0x00, VERSION=0x14
```

이 로그는 ESP32-S3가 CC1101의 상태 레지스터를 실제로 읽었다는 의미다. 다음과 같은 값은 배선이나 SPI 응답을 우선 확인해야 한다.

- 매번 `0xFF`: MISO가 풀업 상태이거나 CC1101이 응답하지 않을 가능성
- 매번 `0x00` 두 개: MISO 고정, CS/SCLK 문제 또는 잘못된 읽기 명령 가능성
- 실행할 때마다 값이 변함: 전원, GND, 배선 접촉 또는 SPI 신호 무결성 문제 가능성
- `status=4`: CC1101 SO/MISO ready 대기 timeout
- `status=3`: SPI 전송 또는 FIFO 상태 오류

확인 순서:

1. CC1101 전원을 3.3 V로 공급했는지 확인
2. 두 보드와 CC1101의 GND가 공통인지 확인
3. SCLK 12, MOSI 11, MISO 13, CS 14 배선 확인
4. CS가 다른 장치와 공유되지 않는지 확인
5. SPI 속도를 1 MHz로 유지한 상태에서 재검증
6. 가능하면 로직 애널라이저로 CS 하강 후 SCLK와 MISO 응답 확인

### 6.5 TX는 성공하지만 RX 출력이 없는 경우

`TX PASS`는 TX FIFO가 정상적으로 비워졌다는 뜻이지 상대 보드가 수신했다는 뜻은 아니다. 다음 항목을 양쪽 보드에서 비교한다.

- 동일한 433 MHz CC1101 모듈인지
- 안테나가 연결되어 있는지
- `FREQ2/FREQ1/FREQ0`, 변조, 데이터 속도, sync word가 동일한지
- TX와 RX가 같은 `CHANNR`에 머물렀는지
- RX 소스가 실제로 RX 역할로 빌드됐는지

패킷에 채널 번호를 포함했기 때문에 다음 두 값도 비교할 수 있다.

```text
channel=20 payload="FHSS:671:20:2"
```

로그의 현재 수신 채널과 payload 내부 송신 채널이 같아야 한다. 서로 다르면 로그 생성 또는 슬롯/채널 관리 코드부터 점검한다.

### 6.6 일부 홉 채널만 계속 누락되는 경우

이번 테스트에서는 RX 체류 시간 200 ms를 사용했을 때 채널 0과 10만 수신되고 채널 20은 계속 누락됐다. 채널 20 RF 자체의 문제처럼 보일 수 있지만, 원인은 주기 간 위상 고정이었다.

```text
TX: 채널당 3회 × 100 ms = 300 ms
RX: 채널당 200 ms
```

두 주기가 일정한 공통 패턴을 만들면서 RX가 채널 20을 청취할 때 TX는 반복적으로 다른 채널에 있었다. RX 체류 시간을 137 ms로 변경하자 채널 0, 10, 20이 모두 수신됐다.

이 문제를 디버깅할 때는 다음 로그를 최소 10초 이상 수집하고 채널별 성공 횟수를 센다.

```powershell
idf.py -p COM5 monitor
```

판단 기준:

- 특정 채널만 0회: 먼저 검색/송신 주기의 위상 관계 확인
- 특정 채널의 CRC 실패만 증가: 해당 주파수의 간섭이나 주파수 오차 확인
- 모든 채널이 불규칙하게 누락: 전원, 안테나, 거리, RX FIFO 처리 확인
- 채널 변경 직후만 실패: IDLE 전환, 보정 시간 및 RX 진입 순서 확인

### 6.7 CRC, RSSI, LQI 로그 해석

```text
FHSS RX PASS: ... RSSI=-59 dBm LQI=12
```

- `PASS`: CC1101이 패킷 CRC를 정상으로 판정
- `CRC_FAIL`: 패킷은 감지했지만 내용이 손상됨
- RSSI: 수신 신호 세기의 상대적인 진단값. 0에 가까울수록 강함
- LQI: 현재 구현에서는 CC1101 상태 바이트의 7비트 값을 그대로 출력

RSSI와 LQI 하나만으로 성공 여부를 판단하지 않는다. 가장 중요한 기준은 CRC를 통과한 payload가 올바른 순서와 채널에서 반복 수신되는지다.

### 6.8 Watchdog Backtrace가 발생하는 경우

다음과 같은 backtrace는 CC1101 통신 실패 로그가 아니다.

```text
task_wdt_timeout_handling
vTaskDelay
ptt_button_task
```

기존 FSM과 주변장치 태스크가 함께 실행되면서 발생한 별도 태스크 watchdog 문제다. 현재 Smoke Test에서는 `CC1101_SMOKE_TEST_ONLY`를 활성화해 FSM 시작을 보류한다. Backtrace의 마지막 애플리케이션 함수가 `rf_transport`인지 다른 컴포넌트인지 확인해 문제 범위를 분리한다.

### 6.9 디버깅 로그 개선 방향

현재 로그를 다음 구조로 통일하면 비교가 쉬워진다.

```text
[RX] result=PASS local_slot=164 tx_slot=671 channel=20 repeat=2 rssi=-59 lqi=12
```

후속 구현에서 추가할 진단값:

- 채널별 TX/RX 성공 횟수
- CRC 실패 및 timeout 횟수
- 마지막 정상 수신 이후 경과 시간
- 예상 슬롯과 실제 수신 슬롯 차이
- 채널 전환 소요 시간
- 동기 상태와 상태 전이 원인

이 통계가 있으면 단순 RF 손실, 특정 채널 간섭, 슬롯 동기 오차를 로그만으로 구분하기 쉬워진다.

## 7. 현재 한계와 다음 작업

- RX는 TX와 시간 동기화된 상태가 아니라 세 채널을 순환 검색한다.
- 패킷의 TX 슬롯과 RX의 `scan_slot`은 서로 다른 로컬 카운터다.
- ACK, 재전송 및 패킷 손실률 통계는 아직 없다.
- GDO 인터럽트 대신 SPI polling을 사용한다.
- 현재 테스트 로직은 `main.c`에 임시로 들어 있다.

다음 브랜치에서는 `fhss_service`와 `fhss_fsm`을 추가해 `main.c`를 단순화하고, SYNC 패킷의 슬롯 번호와 수신 시각을 이용해 TX/RX 슬롯 경계를 맞춘다.
