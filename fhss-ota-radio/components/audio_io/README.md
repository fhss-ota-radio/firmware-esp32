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
- 마이크는 `audio_io_init()` 시점에 바로 enable, **스피커는 채널만 만들고 enable 안 함** — `audio_io_speaker_enable()`/`disable()`로 실제 재생 시점(RX_AUDIO 진입/이탈)에만 켠다. TX DMA를 켜둔 채 한 번도 안 쓰면 GDMA TX 인터럽트가 NULL 컨텍스트로 불려 재부팅되는 문제가 실기기에서 확인됨(2026-08-10) — 마이크는 계속 캡처해도 문제없어서 그대로 둠
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

// RX_AUDIO 진입 시 스피커 켜기, 이탈 시 끄기 (main/fsm.c의 on_enter_rx_audio/on_enter_menu_idle 참고)
audio_io_speaker_enable();
audio_io_decode_play(rx_frame, rx_len);
audio_io_speaker_disable();

// 테스트용: RX_AUDIO 미구현 상태에서 앰프 배선/동작 확인용 삐빅음
// (main/fsm.c의 tx_audio_task, PTT 누르면 재생)
audio_io_speaker_enable();
audio_io_play_beep();
audio_io_speaker_disable();
```

## 하드웨어 배선 확정 시 수정할 것 (`audio_io_config.h`)

- `AUDIO_IO_MIC_*_GPIO` (WS/BCLK/SD) — INMP441, 현재 GPIO4/5/6 (2026-08-11 재배정)
- `AUDIO_IO_SPK_*_GPIO` (BCLK/WS(LRC)/DOUT(DIN)/GAIN/SD) — MAX98357A, 현재 GPIO15/7/16/17/18(2026-08-14 재배선, CC1101과 간섭 있어 LRC/BCLK/DIN 재이동. 최초 재배정은 2026-08-11 — GPIO9~14를 CC1101 SPI+GDO0+GDO2용으로 통째로 비워두기 위함). CC1101 핀 블록(SCLK12/MOSI11/MISO13/CS14/GDO0 10)과 안 겹침
- INMP441의 L/R 핀은 3V3 고정(우채널) 배선 — `audio_io.c`에서 `I2S_STD_SLOT_RIGHT`로 수신. 다르게 배선하면 `I2S_STD_SLOT_LEFT`로 변경
- MAX98357A의 SD/GAIN 핀은 이제 GPIO 직결 — VDD/GND 직결 배선이면 안 됨(GPIO가 직접 제어함). GAIN을 다른 값으로 바꾸려면 `spk_channel_init()`의 `gpio_set_level(AUDIO_IO_SPK_GAIN_GPIO, ...)` 수정

## LOOPBACK_ENABLE (임시 마이크 테스트, 검증 끝나면 제거)

`audio_io_config.h`의 `LOOPBACK_ENABLE` 매크로(기본 꺼짐, 주석 처리)를 켜면
`audio_io_decode_play_scaled(data, len, gain)`/`audio_io_decode_peek_peak(data, len)`
두 함수가 컴파일에 포함된다. `main/fsm.c`의 `mic_test_task`(MENU_IDLE에서
PTT로 mic->Speex->스피커 loopback)가 이 함수들을 사용 — INMP441 실배선
확인용 임시 코드. 자세한 배경/실측값은 로컬
`~/Documents/projects/kcci-final/troubleshoot/mic_loopback_test-inmp441_low_amplitude.md`
참고(git 비관리).

## 제약 / TODO

- `audio_io_capture_encode`/`decode_play`는 한 번에 한 프레임(20ms)만 처리 — 호출 주기 관리(태스크/타이머)는 상위(FSM TX_AUDIO/RX_AUDIO)에서 담당
- `audio_io_decode_play()`/`audio_io_play_beep()` 호출 전 반드시 `audio_io_speaker_enable()`을 먼저 불러야 함 — 안 그러면 채널이 READY 상태라 `i2s_channel_write()`가 실패함
- `AUDIO_IO_BEEP_AMPLITUDE`(500/32767)와 GAIN 고정값(6dB)로 볼륨을 이중으로 낮춰둔 상태 — 정식 음량은 나중에 조정 필요
- 캡처 품질(마이크 게인 등)은 실기기 브링업 중이라 미검증