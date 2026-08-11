#pragma once

#include <stddef.h>
#include <stdint.h>

#include "audio_io_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 마이크(I2S_NUM_0)/스피커(I2S_NUM_1) 채널 초기화. app_main 등에서 한 번 호출.
 * audio_codec_init()도 별도로 호출해야 한다 — 이 컴포넌트는 I2S 입출력만 담당.
 *
 * 마이크는 여기서 바로 enable되지만, 스피커는 채널만 만들어두고 enable하지
 * 않는다 — 실제로 재생할 때만 audio_io_speaker_enable()로 켤 것. TX DMA를
 * 켜두고 한 번도 안 쓰는 채로 방치하면 GDMA TX 인터럽트가 죽는 문제가
 * 실기기에서 확인됐다(2026-08-10).
 */
void audio_io_init(void);

/* 스피커 채널(I2S DMA + SD 핀)을 켠다/끈다. RX_AUDIO 진입/이탈, 또는
 * audio_io_play_beep() 호출 전후에만 쓸 것 — 부팅 시 자동으로 켜두지 않는
 * 이유는 audio_io_init() 주석 참고. disable은 이미 꺼진 상태에서 불러도
 * 안전(에러 무시). */
void audio_io_speaker_enable(void);
void audio_io_speaker_disable(void);

/*
 * PTT 눌렀을 때 "말하기 시작"을 알리는 짧은 2음 삐빅음을 재생한다(블로킹,
 * 총 ~210ms). 호출 전에 audio_io_speaker_enable()이 먼저 호출돼 있어야 한다.
 * RX_AUDIO(수신 재생)가 아직 미구현이라 앰프 배선/동작을 확인하기 위한
 * 테스트용 — 나중에 실제 음성 재생 붙으면 그대로 둬도 되고 빼도 된다.
 */
void audio_io_play_beep(void);

/*
 * 마이크에서 한 프레임(AUDIO_CODEC_FRAME_SAMPLES=160 샘플, 20ms)을 읽어
 * Speex로 인코딩한다. out/out_capacity는 audio_codec_encode()에 그대로 전달됨
 * (AUDIO_CODEC_MAX_ENCODED_BYTES 이상 권장).
 * 반환값: 인코딩된 바이트 수, 실패 시 -1 (I2S 타임아웃 포함).
 */
int audio_io_capture_encode(uint8_t *out, size_t out_capacity);

/*
 * 인코딩된 프레임 하나를 Speex로 디코딩해 스피커로 재생한다.
 * data/len: audio_codec_encode()가 만든 프레임 하나 분량.
 * 반환값: 성공 시 0, 실패 시 -1.
 */
int audio_io_decode_play(const uint8_t *data, size_t len);

/*
 * TEMP(마이크 실배선 테스트용, 검증 끝나면 제거): audio_io_decode_play()와
 * 동일하지만, 디코딩한 PCM에 (amplitude_cap/32767) 배율을 곱해 소프트웨어로
 * 감쇠한 뒤 재생한다. amplitude_cap=AUDIO_IO_BEEP_AMPLITUDE(500)를 넘겨주면
 * 삐빅음과 같은 배율로 작게 재생된다.
 */
int audio_io_decode_play_scaled(const uint8_t *data, size_t len, int16_t amplitude_cap);

#ifdef __cplusplus
}
#endif
