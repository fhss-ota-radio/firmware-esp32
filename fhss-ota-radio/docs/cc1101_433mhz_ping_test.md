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

## 6. 현재 한계와 다음 작업

- RX는 TX와 시간 동기화된 상태가 아니라 세 채널을 순환 검색한다.
- 패킷의 TX 슬롯과 RX의 `scan_slot`은 서로 다른 로컬 카운터다.
- ACK, 재전송 및 패킷 손실률 통계는 아직 없다.
- GDO 인터럽트 대신 SPI polling을 사용한다.
- 현재 테스트 로직은 `main.c`에 임시로 들어 있다.

다음 브랜치에서는 `fhss_service`와 `fhss_fsm`을 추가해 `main.c`를 단순화하고, SYNC 패킷의 슬롯 번호와 수신 시각을 이용해 TX/RX 슬롯 경계를 맞춘다.
