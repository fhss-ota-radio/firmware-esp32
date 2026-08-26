# FHSS 무전기 통합 트러블슈팅 총정리

ESP32-S3, CC1101, Speex, I2S, FreeRTOS FSM, Web Serial 모니터를 통합하면서 발생한 문제를 `문제 상황 → 원인 → 해결 → 결과` 순서로 정리한다. 실기기 로그로 확인된 내용과 아직 별도 브랜치에서 검증 중인 내용은 구분한다.

## 1. CC1101 SPI 통신 실패

### 문제 상황

```text
CC1101 PARTNUM=0x00 VERSION=0x00
CC1101 PARTNUM=0xFF VERSION=0xFF
CC1101 register read-back failed: IOCFG2 expected=0x29 actual=0x00
```

보드와 CC1101 모듈, 점퍼선을 바꿔도 초기화가 실패하고 FSM이 `ERROR`로 전이했다.

### 원인

- 초기 핀맵이 실제 통합 보드 배선 및 오디오 주변장치 핀과 충돌했다.
- 단독 SPI 진단과 통합 펌웨어의 SPI 속도가 달라, 단독 시험은 성공하지만 통합 환경에서는 신호가 불안정했다.
- `0x00`은 MISO가 LOW에 고정된 상태, `0xFF`는 MISO가 HIGH/부동인 상태에서 주로 관측됐다.

### 해결

- 최종 CC1101 SPI 핀을 다음과 같이 통일했다.

| CC1101 | ESP32-S3 |
|---|---:|
| SCLK | GPIO12 |
| MOSI | GPIO9 |
| MISO | GPIO11 |
| CS | GPIO10 |
| GDO0 | GPIO13 |

- MOSI–MISO loopback으로 ESP32 SPI 자체를 먼저 검증했다.
- 통합 SPI clock을 배선 환경에서 안정적인 값으로 낮추고 register write/read-back 검증을 추가했다.

### 결과

```text
PARTNUM=0x00 VERSION=0x14
CC1101 register read-back OK: IOCFG2=0x29
```

양쪽 보드에서 CC1101 초기화와 레지스터 설정이 정상 동작했다.

## 2. 단독 진단만 실행되고 FSM·OLED·오디오가 실행되지 않음

### 문제 상황

```text
CC1101 mode: ... FSM/OLED/audio skipped
```

SPI 검사는 성공하지만 통합 무전기 기능이 전혀 시작되지 않았다.

### 원인

`main/main.c`의 CC1101 standalone/loopback 진단 모드가 활성화된 펌웨어였다.

### 해결

SPI 검증 후 진단 모드를 해제하고 정상 `fsm_init()` 경로를 복원했다.

### 결과

OLED, PTT, 로터리, 오디오, FHSS service가 함께 초기화되고 `BOOT_INIT → MENU_COMM`으로 진입했다.

## 3. GDO0 timestamp가 없거나 TX timeout 발생

### 문제 상황

```text
rf_transport: TX timeout
fhss_service: SYNC TX failed
```

또는 RX timestamp가 계속 0으로 기록됐다.

### 원인

- 코드의 GDO0 GPIO와 실제 배선이 일치하지 않았다.
- CC1101이 RX 상태가 아닌 IDLE 상태에서 GDO0 상승 에지를 기다린 경우 sync word를 검출할 수 없었다.
- SPI/CS/MISO 실패가 GDO 문제처럼 보인 경우도 있었다.

### 해결

- GDO0를 최종 GPIO13으로 통일했다.
- GDO 이벤트를 기다리기 전에 CC1101을 명시적으로 RX 상태로 전환했다.
- GDO 문제를 판단하기 전에 PARTNUM/VERSION과 register read-back을 먼저 확인했다.

### 결과

GDO0 ISR에서 `esp_timer_get_time()` timestamp를 확보하고, 실제 무선 sync word 검출 시각을 동기 기준으로 사용할 수 있게 됐다.

## 4. ISR queue assert와 반복 재부팅

### 문제 상황

```text
assert failed: xQueueGenericSendFromISR
```

### 원인

임시 `rf_transport` 객체 주소로 ISR을 등록한 후 객체를 서비스 구조체로 복사해, ISR이 사라진 스택 객체와 queue를 참조했다.

### 해결

- 최종 서비스 객체 내부의 `rf_transport`를 직접 초기화했다.
- ISR 등록 이후 해당 객체를 복사하거나 이동하지 않도록 했다.

### 결과

ISR queue assert와 반복 부팅이 사라졌다.

## 5. 설정은 300ms인데 슬롯 간격이 약 302.7ms로 증가

### 문제 상황

매 슬롯이 설정값보다 길어지고 시간이 지날수록 TX/RX 기준이 밀렸다.

### 원인

TX가 매번 GDO0 송신 timestamp를 새로운 기준 시각으로 설정했다. 약 2.7ms의 sync word 송신 지연이 슬롯마다 누적됐다.

### 해결

- TX는 최초 고정 시간 기준을 계속 유지했다.
- TX GDO0 timestamp는 진단 측정에만 사용했다.
- RX만 실제 수신 timestamp로 timing error를 계산하고 제한적으로 보정했다.

### 결과

송신 지연 누적이 제거되고 슬롯 주기가 설정값을 따라갔다.

## 6. Timing window의 early 경계가 설정과 다름

### 문제 상황

early/late margin을 다르게 설정했는데 한쪽 경계 판정이 예상과 달랐다.

### 원인

early 경계를 검사하는 코드가 `early_margin_us` 대신 `late_margin_us`를 사용했다.

### 해결

early 경계에 `early_margin_us`를 사용하도록 수정했다.

### 결과

비대칭 timing window 설정이 의도대로 동작했다.

## 7. 동기 실패 시 즉시 처음부터 재탐색해 복구가 느림

### 문제 상황

일시적인 패킷 손실에도 `SEARCHING`으로 되돌아가 연결 단절 시간이 길어졌다.

### 원인

수신 실패를 짧은 페이딩과 완전한 동기 상실로 구분하지 않고 동일하게 처리했다.

### 해결

- 성공/실패 연속 횟수와 recovery 상태를 추가했다.
- 일시적인 miss에서는 기존 슬롯 예측을 유지했다.
- 실제 timestamp error는 한 번에 최대 500us만 점진 보정했다.
- 복구 제한을 넘을 때만 rendezvous 채널에서 다시 탐색하도록 했다.

### 결과

짧은 손실에서 전체 재탐색 없이 추종을 유지할 수 있게 됐다. 장시간 A/B 평가는 `fhss_algorithm_ab_test.md` 기준으로 계속 측정한다.

## 8. FreeRTOS watchdog 및 polling task CPU 점유

### 문제 상황

```text
task_wdt: IDLE0 did not reset
CPU 0: ptt_button
CPU 1: rotary_encoder
```

### 원인

`CONFIG_FREERTOS_HZ=100`에서 2ms 또는 5ms를 `pdMS_TO_TICKS()`로 변환하면 0 tick이 되어 polling task가 busy-loop했다.

### 해결

```c
const TickType_t ticks = pdMS_TO_TICKS(POLL_MS);
vTaskDelay(ticks > 0U ? ticks : 1U);
```

빌드 설정도 `CONFIG_FREERTOS_HZ=1000`으로 통일했다.

### 결과

PTT/로터리 task의 CPU 독점과 watchdog 재발이 사라졌다.

## 9. N16R8/N8R8 보드 교체 후 부팅 또는 flash 실패

### 문제 상황

- 실제 8MB 보드에 16MB 이미지 설정이 남았다.
- `idf.py fullclean` 후에도 이전 flash 크기가 유지됐다.
- 부팅 초기에 주변장치 초기화 전 재부팅하거나 flash가 실패했다.

### 원인

`fullclean`은 `build/`만 지우며 기존 `sdkconfig`는 삭제하지 않는다. 따라서 이전 보드의 flash size가 계속 사용됐다.

### 해결

보드 전환 시 반드시 `sdkconfig`를 먼저 삭제했다.

```powershell
Remove-Item .\sdkconfig -Force
idf.py fullclean
```

- N16R8/COM6: `sdkconfig.defaults`
- N8R8/COM8: `sdkconfig.defaults;sdkconfig.defaults.n8r8`

### 결과

COM6에는 16MB, COM8에는 8MB 이미지가 각각 정상 빌드·플래시되고 hash 검증을 통과했다.

## 10. Speex 소스가 없어 빌드 실패

### 문제 상황

새 clone 환경에서 `components/audio_codec/speex`가 비어 있어 codec 빌드가 실패했다.

### 원인

Speex는 일반 디렉터리가 아니라 git submodule이다.

### 해결

```bash
git submodule update --init --recursive
```

### 결과

동일 Speex 버전으로 재현 가능한 빌드가 가능해졌다.

## 11. COM 포트 access denied 및 Web Serial 연결 끊김

### 문제 상황

```text
PermissionError(13, '액세스가 거부되었습니다.')
BufferOverrunError: Buffer overrun
```

### 원인

- ESP-IDF monitor와 브라우저 Web Serial이 같은 COM 포트를 동시에 열었다.
- 대량 시리얼 로그를 브라우저가 처리하지 못해 수신 버퍼가 넘쳤다.

### 해결

- 한 포트는 한 프로그램만 사용하도록 monitor를 먼저 종료했다.
- HTML에 연결 해제/재연결과 파싱 실패 카운터를 추가했다.
- 오디오 패킷별 HEX dump를 제거하고 슬롯당 한 줄 `FHSS_MON` 로그로 축약했다.

### 결과

Web Serial 연결 안정성이 개선되고 실시간 채널 그래프를 유지하면서 로그 부하가 크게 줄었다.

## 12. PTT를 누르고 있는데 RX_AUDIO가 중간에 종료됨

### 문제 상황

TX가 PTT를 계속 누르는 동안 RX 화면과 재생이 `MENU_COMM`으로 돌아갔다.

### 원인

RX audio queue에 새 frame이 1초 동안 없으면 상대 PTT가 해제됐다고 간주했다. FHSS 손실이나 채널 복구 중 발생한 일시적인 frame 공백도 세션 종료로 오판했다.

### 해결

- 일시적인 queue 공백에는 20ms 단위로 silence를 출력하며 `RX_AUDIO`를 유지했다.
- 정상 종료는 명시적인 `TALKSPURT_END → RX_DONE`으로 처리했다.
- 장시간 무선 단절만 `SYNC_LOST`로 처리했다.

### 결과

별도 `fix/fhss-rx-continuity` 브랜치에서 PTT 유지 중 조기 종료를 방지했다. develop 원본과 비교 시험할 수 있도록 수정은 분리해 보존했다.

## 13. PTT를 놓았는데 RX_AUDIO가 계속 유지됨

### 문제 상황

송신 측 PTT를 놓고 END packet을 보냈는데 수신 UI가 RX 상태에 머물렀다.

### 원인

20ms 오디오 frame마다 FSM의 `RX_FRAME` 이벤트도 함께 올렸다. 깊이 16의 FSM event queue가 불필요한 self-loop 이벤트로 가득 차면서 중요한 `RX_DONE`이 조용히 유실될 수 있었다.

### 해결

- `RX_FRAME`은 `MENU_COMM → RX_AUDIO` 진입 시 한 번만 올렸다.
- RX_AUDIO 진입 후 데이터는 전용 audio queue로만 전달했다.
- FSM event queue 전송 실패를 경고 로그로 남겼다.

### 결과

별도 `fix/fhss-rx-continuity` 브랜치에서 END 처리 신뢰성을 높였다.

## 14. PTT 해제 과정에서 I2S가 점유된 채 남음

### 문제 상황

통화 후 다음 녹음/재생에서 `ESP_ERR_INVALID_STATE`, 무음 또는 비정상 출력이 발생했다.

### 원인

PTT 해제나 상태 전이에서 audio task를 강제 삭제해, blocking I2S 호출이 정상 종료되고 channel이 disable되기 전에 task 수명이 끝났다.

### 해결

- 강제 `vTaskDelete()` 대신 stop flag를 전달했다.
- task가 blocking call에서 빠져나온 뒤 I2S를 disable하고 스스로 종료하도록 했다.
- 다음 세션 진입 시 필요한 I2S channel을 정상 enable했다.

### 결과

반복 PTT 시험에서 I2S 점유 상태가 남는 문제를 줄이고 세션 재진입이 가능해졌다.

## 15. RX idle 구간에서 두두두 반복음 발생

### 문제 상황

수신 frame이 잠깐 없을 때 마지막 파형이 반복되는 잡음이 들렸다.

### 원인

I2S TX가 circular DMA로 동작하는 동안 새로운 sample write가 멈춰 마지막 buffer가 반복 재생됐다.

### 해결

RX task가 20ms마다 queue를 확인하고 frame이 없으면 silence frame을 I2S에 기록했다.

### 결과

일시적인 RF 공백에서 마지막 음성 조각이 반복되는 잡음이 감소했다.

## 16. FHSS 통화 음성이 심하게 깨짐

### 문제 상황

실제 통화에서 음성이 끊기고 다음 로그가 반복됐다.

```text
audio TX queue full
encoded audio frame dropped: length=20
```

### 원인

오디오 RF packet을 보낼 때마다 `OTA_DIAG`가 49바이트 payload 전체를 115200 baud UART로 HEX dump했다. 동기식 로그 출력이 실시간 송신 경로를 늦춰 깊이 8의 TX queue가 가득 찼고, Speex frame이 RF 송신 전에 버려졌다. HTML 자체가 RF에 개입한 것이 아니라 HTML용으로 과도하게 출력한 펌웨어 로그가 병목이었다.

### 해결

- 오디오 DATA와 `SEED_ANNOUNCE`의 상세 metadata/HEX dump를 억제했다.
- OTA protocol packet과 SYNC 진단은 유지했다.
- HTML용 채널 정보는 슬롯당 한 줄만 출력했다.

```text
FHSS_MON: TX slot=3 channel=60
FHSS_MON: RX slot=3 channel=60
```

- HTML은 새 경량 형식과 기존 로그 형식을 모두 지원하도록 했다.

### 결과

`demo/fhss-live-audio-monitor` 브랜치에서 HTML을 연결한 상태로 실제 음성 통화가 성공했다. 시리얼 관찰 기능을 유지하면서 TX queue drop을 유발하던 로그 부하를 제거했다.

## 17. 스피커 출력이 너무 작음

### 문제 상황

통신은 성공하지만 시연 환경에서 스피커 음량이 작았다.

### 원인

MAX98357A GAIN GPIO가 HIGH(VDD)로 설정되어 6dB gain을 사용했다.

### 해결

시연용 브랜치에서 GAIN GPIO를 LOW(GND)로 변경해 12dB로 높였다.

```c
gpio_set_level(AUDIO_IO_SPK_GAIN_GPIO, 0);
```

### 결과

기존 대비 hardware gain이 6dB 증가했다. 빌드는 성공했으며 실제 음량과 clipping 여부는 시연 장비에서 최종 확인한다.

## 18. 빠른 판정 순서

문제가 재발하면 다음 순서로 확인한다.

1. 보드 종류와 `sdkconfig`의 flash size, `CONFIG_FREERTOS_HZ=1000` 확인
2. CC1101 `PARTNUM=0x00 VERSION=0x14` 확인
3. `IOCFG2` register read-back 확인
4. TX/RX의 `secret_seed.txt` 값 일치 확인
5. 양쪽 FSM이 `MENU_COMM`인지 확인
6. `FHSS_MON`에서 동일 slot/channel 여부 확인
7. `audio TX queue full`, `encoded audio frame dropped` 확인
8. RX의 `audio packet gap`, timing error, `SYNC_LOST` 확인
9. PTT 해제 후 `TALKSPURT_END`, `RX_DONE`, `MENU_COMM` 복귀 확인
10. COM 포트를 monitor와 HTML이 동시에 점유하지 않는지 확인

## 현재 검증 기준

- develop 기준 펌웨어 통합 빌드 성공
- COM6 N16R8/16MB, COM8 N8R8/8MB 분리 빌드·플래시 성공
- CC1101 SPI/register read-back 성공
- seed announce 및 세션별 HMAC hop seed 파생 확인
- TX/RX seed 기반 채널 호핑 확인
- Speex 2 frame/RF packet 음성 통화 성공
- 경량 Web Serial 채널 모니터와 음성 통화 동시 시연 성공

