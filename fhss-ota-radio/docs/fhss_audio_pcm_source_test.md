# 마이크 없는 FHSS 오디오 End-to-End 테스트

## 목적

INMP441 마이크가 준비되지 않은 상태에서 마이크 드라이버만 우회하고 다음 실제
제품 경로를 검증한다.

```text
8 kHz mono signed-16 PCM test source
  → audio_codec_encode() (Speex, 160 samples/20 ms)
  → fhss_audio_adapter (2 frames/40 ms RF packet)
  → CC1101 synchronized FHSS
  → 상대 CC1101
  → fhss_audio_adapter packet unpack
  → audio_codec_decode()
  → Speaker
```

현재 내장 소스는 500 Hz와 750 Hz 패턴 및 짧은 무음을 반복한다. 파일 시스템이나
WAV 파서 없이 codec/RF/speaker 경로를 먼저 확인하기 위한 진단 신호다. 실제 사람
음성 `test_voice.pcm` 배열은 이후 동일한 160-sample 공급 위치에 교체할 수 있다.

## 활성화

`main/fsm.c`의 다음 값을 사용한다.

```c
#define FHSS_AUDIO_PCM_TEST_ENABLED 1
```

- `1`: Mic 대신 내장 PCM 패턴 사용
- `0`: 기존 `audio_io_capture_encode()` Mic 경로 사용

두 보드 모두 같은 펌웨어를 사용한다. 평소에는 RX 대기하며, 송신 보드에서 PTT를
누르고 있는 동안만 PCM 패턴을 생성해 전송한다.

## 데이터 조건과 주기

| 항목 | 값 |
|---|---:|
| Sample rate | 8,000 Hz |
| Channel | Mono |
| PCM | Signed 16-bit little-endian |
| Samples/frame | 160 |
| PCM bytes/frame | 320 bytes |
| Frame period | 20 ms |
| Frames/RF packet | 2 |
| RF audio duration/packet | 40 ms |

`vTaskDelayUntil()`을 사용하므로 encode와 로그 실행 시간이 다음 프레임 주기에
누적되지 않는다. 현재 `CONFIG_FREERTOS_HZ=100`에서는 20 ms가 정확히 2 tick이다.

## 실행 로그

송신 시작:

```text
PCM TEST MODE: microphone bypassed; real Speex/FHSS/RF path active
START format=8000Hz mono s16le samples=160 frame_ms=20 source=500/750Hz-pattern
```

1초마다 PCM/encode/submit 통계를 출력한다.

```text
FRAME frame=49 pcm_peak=8000 encoded_bytes=20 submitted=50 encode_fail=0 submit_fail=0 interval_us[min/max]=19990/20015
AUDIO_TX packet=25 sequence=24 frames=2 bytes=49 flags=0x00
```

수신 보드는 25 packet마다 다음 로그를 출력한다.

```text
AUDIO_RX packet=25 sequence=24 frames=2 bytes=49 flags=0x00
```

PTT 해제 시 최종 통계:

```text
PCM TEST STOP generated=250 encoded=250 submitted=250 encode_fail=0 submit_fail=0 interval_us[min/max]=19990/20015
TX session ended: packets=125; RX standby resumed
```

정상 판정 기준:

- `encoded_bytes=20`이 현재 quality 4 CBR 설정과 일치
- `encode_fail=0`, `submit_fail=0`
- 20 ms 공급 간격이 장시간 한 방향으로 증가하지 않음
- TX/RX packet sequence가 연속적임
- RX 스피커에서 500/750 Hz 패턴과 무음 구간이 반복됨

## 현재 하드웨어 차단 사항

현재 `audio_io_config.h`의 MAX98357A 핀과 CC1101 SPI 핀이 겹친다.

| 기능 | 사용 GPIO |
|---|---|
| CC1101 | GPIO11 MOSI, GPIO12 SCLK, GPIO13 MISO, GPIO14 CS |
| MAX98357A | GPIO11 GAIN, GPIO12 DIN, GPIO13 BCLK, GPIO14 LRC |

따라서 빌드와 TX 로그는 확인할 수 있지만, 이 배선 상태에서 CC1101과 Speaker를
동시에 구동한 결과는 유효하지 않다. 오디오 담당자가 `audio_io_config.h`와 실제
MAX98357A 배선을 충돌 없는 GPIO로 변경한 뒤 두 보드 실기기 판정을 진행한다.
