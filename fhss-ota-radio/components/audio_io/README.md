# audio_io

I2S 마이크(INMP441)/스피커(MAX98357A) 입출력 + audio_codec(Speex) 연결 컴포넌트.

## 구성

| 파일 | 내용 |
|---|---|
| `audio_io_config.h` | 마이크/스피커 I2S 포트·핀 설정. **하드웨어 바뀌면 여기만 수정** |
| `audio_io.h` | 공개 API |
| `audio_io.c` | I2S 채널 초기화 + audio_codec encode/decode 연결 |

## 설계

- 마이크(RX 전용)와 스피커(TX 전용)를 **서로 다른 I2S 포트**(`I2S_NUM_0`/`I2S_NUM_1`)에 고정 배정
  - 반이중 라디오와 무관하게 마이크/스피커는 물리적으로 별도 핀이라 동시 사용 가능
  - TX_AUDIO/RX_AUDIO 전환 때마다 I2S 재설정할 필요 없이 부팅 시 한 번만 초기화
- INMP441은 24bit 샘플을 32bit 슬롯에 실어 보내므로 RX는 `I2S_DATA_BIT_WIDTH_32BIT`로 열고, 상위 16bit만 취해 16bit PCM으로 변환
- MAX98357A는 ESP가 클럭 마스터라 슬롯 폭 제약이 없어 TX는 `I2S_DATA_BIT_WIDTH_16BIT`로 열어 audio_codec의 PCM을 그대로 사용
- 샘플레이트/프레임 크기는 audio_codec 기준(`AUDIO_CODEC_SAMPLE_RATE`=8kHz, `AUDIO_CODEC_FRAME_SAMPLES`=160)에 맞춤 — 리샘플링 없음

## 사용법

```c
#include "audio_io.h"
#include "audio_codec.h"

audio_codec_init();   // Speex 인코더/디코더
audio_io_init();      // 마이크/스피커 I2S 채널

// TX_AUDIO: 20ms마다 한 프레임 캡처+인코딩
uint8_t frame[AUDIO_CODEC_MAX_ENCODED_BYTES];
int n = audio_io_capture_encode(frame, sizeof(frame));
if (n > 0) { /* frame[0..n) 을 rf_transport로 송신 */ }

// RX_AUDIO: 수신한 프레임 디코딩+재생
audio_io_decode_play(rx_frame, rx_len);
```

## 하드웨어 배선 확정 시 수정할 것 (`audio_io_config.h`)

- `AUDIO_IO_MIC_*_GPIO` (BCLK/WS/SD) — INMP441, 현재 GPIO5/6/7은 **placeholder**
- `AUDIO_IO_SPK_*_GPIO` (BCLK/WS/DOUT) — MAX98357A, 현재 GPIO15/16/17은 **placeholder**
- INMP441의 L/R 핀은 GND 고정(좌채널) 배선 가정 — 다르게 배선하면 `audio_io.c`의 `I2S_STD_SLOT_LEFT`를 `I2S_STD_SLOT_RIGHT`로 변경
- MAX98357A의 SD(SHUTDOWN) 핀은 VDD 직결(상시 활성) 가정 — GPIO로 켜고 끄려면 `AUDIO_IO_SPK_SD_GPIO` 정의 후 `audio_io.c`에서 사용

## 제약 / TODO

- `audio_io_capture_encode`/`decode_play`는 한 번에 한 프레임(20ms)만 처리 — 호출 주기 관리(태스크/타이머)는 상위(FSM TX_AUDIO/RX_AUDIO)에서 담당
- `main.c`/FSM에 아직 연결 안 됨 — 컴포넌트 단독 빌드만 가능한 상태
- 실기기 미보유로 캡처 품질(마이크 게인, 앰프 볼륨 등) 미검증

test update.

test update2.