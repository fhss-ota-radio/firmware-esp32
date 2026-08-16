# FHSS·CC1101 통합 테스트 트러블슈팅

## 1. 문서 목적

ESP32-S3 두 대와 CC1101을 사용한 FHSS 오디오 통합 과정에서 확인한 SPI 통신, GDO0 timestamp, 슬롯 동기화 문제와 해결 방법을 정리한다.

검증 환경:

| 항목 | 설정 |
|---|---|
| TX 보드 | COM6, device ID `A29E60` |
| RX 보드 | COM10, device ID `A29DC0` |
| CC1101 핀 | SCLK 12, MOSI 9, MISO 11, CS 10, GDO0 13 |
| 홉 채널 | CC1101 `CHANNR {0, 10, 20}` |
| 슬롯 | 300 ms |
| 채널 전환 guard | 5 ms |
| 오디오 시험 | Speex 프레임 2개/RF 패킷, PCM 패턴 입력 |

## 2. 통합 모드가 실행되지 않는 문제

### 증상

부팅 로그에 다음 메시지가 출력되고 FSM, OLED, 오디오가 실행되지 않았다.

```text
CC1101 mode: ... FSM/OLED/audio skipped
```

### 원인

`main/main.c`의 단독 SPI 진단 플래그가 활성화돼 있었다.

```c
#define CC1101_STANDALONE_DIAGNOSTIC 1
```

### 해결

SPI 단독 검증을 마친 뒤 플래그를 `0`으로 변경해 기존 FSM 경로를 복원했다.

```c
#define CC1101_STANDALONE_DIAGNOSTIC 0
```

정상 통합 부팅에서는 다음 순서가 보여야 한다.

```text
CC1101 pre-config PARTNUM=0x00 VERSION=0x14
CC1101 register read-back OK: IOCFG2=0x29
fhss_audio_adapter: ready: RX standby
fsm: BOOT_INIT -> MENU_COMM
```

## 3. 단독 진단은 성공하지만 통합 모드 SPI가 실패하는 문제

### 증상

10 kHz 단독 진단에서는 정상 식별값을 읽었지만 통합 모드에서는 다음처럼 실패했다.

```text
CC1101 pre-config PARTNUM=0x00 VERSION=0x00
CC1101 register read-back failed: IOCFG2 expected=0x29 actual=0x00
fhss_audio_adapter: FHSS service initialization failed
fsm: MENU_COMM -> ERROR
```

### 원인

단독 진단은 SPI 10 kHz를 사용했지만 `fhss_audio_adapter`의 통합 설정은 1 MHz를 사용했다. 현재 GPIO9~13 배선과 보드 환경에서는 1 MHz 통신이 불안정했다.

이 문제는 태스크 우선순위나 FSM 오류가 아니다. 실패가 서비스 태스크 생성 전 레지스터 read-back에서 발생하므로 SPI 신호 단계의 문제다.

### 해결

통합 시험의 SPI 속도를 우선 100 kHz로 낮췄다.

```c
#define CC1101_SPI_CLOCK_HZ 100000
```

100 kHz에서 두 보드 모두 다음 결과를 확인했다.

```text
PARTNUM=0x00
VERSION=0x14
IOCFG2=0x29
```

100 kHz는 현재 검증용 안전값이다. 최종값은 배선 길이, 신호 품질과 오디오 처리량을 측정하면서 단계적으로 올려야 한다.

## 4. 동기화 오차가 약 한 슬롯 발생하는 문제

### 증상

TX의 SYNC 패킷은 수신됐지만 RX가 `SYNCHRONIZING`에서 벗어나지 못했다.

```text
SYNC RX: state=SYNCHRONIZING ... error=-288570 us
SYNC RX: state=SYNCHRONIZING ... error=-279492 us
```

슬롯 길이가 300 ms이므로 약 `-280 ms` 오차는 단순 clock drift가 아니라 거의 한 슬롯 오래된 timestamp를 사용한 증상이다.

### 원인

한 슬롯에서 TX는 SYNC 이후 여러 오디오 패킷을 보낸다. GDO0 ISR은 각 패킷의 sync word 검출 시각을 timestamp 큐에 기록한다.

채널 변경 시 기존 구현은 다음 항목만 초기화했다.

- CC1101 RX FIFO
- CC1101 TX FIFO
- `CHANNR`

그러나 이전 채널에서 기록된 GDO0 timestamp는 남아 있을 수 있었다. 이 오래된 timestamp가 다음 슬롯의 SYNC 패킷과 결합되면서 큰 음수 오차가 발생했다.

```text
이전 채널의 GDO0 timestamp
            +
다음 채널의 SYNC payload
            ↓
약 한 슬롯 빠른 수신으로 잘못 판정
```

### 해결

`rf_transport_set_channel()`이 채널 변경과 FIFO flush에 성공하면 timestamp 큐도 초기화하도록 수정했다.

```c
if (status == RF_TRANSPORT_STATUS_OK &&
    transport->rx_timestamp_queue != NULL) {
    xQueueReset((QueueHandle_t)transport->rx_timestamp_queue);
}
```

중요: 이 처리는 CC1101 최초 설정 함수가 아니라 실제로 매 홉마다 호출되는 `rf_transport_set_channel()`에 있어야 한다.

### 검증 결과

수정 후 COM10이 정상적으로 `TRACKING`에 진입했다.

```text
SYNC RX: state=TRACKING slot=33 channel=0  error=56 us
SYNC RX: state=TRACKING slot=34 channel=10 error=-45 us
SYNC RX: state=TRACKING slot=35 channel=20 error=-12 us
```

- 슬롯 번호가 1씩 증가
- 채널이 `0 → 10 → 20` 순환
- 일반적인 timing error 약 `±70 us`
- 약 12초 동안 TRACKING 유지

## 5. 오디오 RF 경로 검증 결과

COM6에서 PTT를 누르면 PCM 테스트 소스가 실제 Speex/FHSS/RF 경로로 전달됐다.

```text
fsm: MENU_COMM -> TX_AUDIO
fhss_audio_adapter: TX session started
fhss_audio_adapter: AUDIO_TX packet=250 ... frames=2 bytes=49
```

COM10에서는 동기 추종 후 오디오 수신 상태 진입을 확인했다.

```text
fhss_service: SYNC RX: state=TRACKING ...
fsm: MENU_COMM -> RX_AUDIO
```

따라서 다음 경로는 실기기에서 동작했다.

```text
PCM test source
  → Speex encode
  → 오디오 프레임 2개 packetization
  → CC1101 synchronized hopping TX
  → CC1101 synchronized hopping RX
  → RX_AUDIO 진입
```

## 6. 아직 남은 오디오 packet gap

### 증상

RX_AUDIO 진입 후 패킷 sequence가 연속되지 않았다.

```text
audio packet gap: expected=453 received=459
audio packet gap: expected=460 received=467
audio packet gap: expected=475 received=482
```

### 현재 추정 원인

RX 서비스는 SYNC를 처리한 뒤 다음 슬롯의 채널 전환 시각까지 대기한다. 이 구조에서는 같은 슬롯에서 SYNC 이후 도착하는 여러 오디오 패킷을 계속 drain하지 못할 수 있다.

SPI 100 kHz의 처리량도 손실에 영향을 줄 수 있으나, 현재 로그상 RX 슬롯 처리 구조를 먼저 점검해야 한다.

### 다음 수정 방향

1. SYNC 수신 후 다음 채널 전환 5 ms 전까지 현재 채널에서 데이터 패킷을 반복 수신한다.
2. 다음 switch deadline을 넘기지 않도록 남은 시간 기반 timeout을 사용한다.
3. SYNC 패킷과 DATA 패킷 통계를 분리한다.
4. 채널별 오디오 sequence gap과 FIFO overflow를 기록한다.
5. 안정화 후 SPI 속도를 100 kHz에서 단계적으로 높여 최대 안정값을 측정한다.

## 7. 재현 및 판정 절차

1. COM6과 COM10에 같은 펌웨어를 플래시한다.
2. 양쪽에서 다음 초기화 로그를 확인한다.

```text
VERSION=0x14
register read-back OK
ready: RX standby
MENU_COMM
```

3. COM6의 PTT를 약 5초 누른다.
4. COM6에서 `TX_AUDIO`, `AUDIO_TX`를 확인한다.
5. COM10에서 `SYNCHRONIZING → TRACKING`, `RX_AUDIO`를 확인한다.
6. timing error가 ±5 ms 이내인지 확인한다.
7. `SERVICE_ERROR`, `SYNC_LOST`, packet gap 발생 여부를 기록한다.

## 8. 관련 Git 정보

- 브랜치: `feature/test/fhss-audio-develop-sync-test`
- 커밋: `312b758`
- 커밋 내용:
  - CC1101 단독 진단 모드 해제
  - 통합 SPI 속도 100 kHz 적용
  - 채널 전환 시 GDO0 timestamp 큐 초기화

