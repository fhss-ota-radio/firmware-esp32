#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Speex 협대역(narrowband) 고정: 8kHz, 20ms(160 샘플) 프레임, 16-bit mono PCM. */
#define AUDIO_CODEC_SAMPLE_RATE       8000
#define AUDIO_CODEC_FRAME_SAMPLES     160
#define AUDIO_CODEC_MAX_ENCODED_BYTES 64

/* 인코더/디코더 상태를 생성한다. app_main()에서 한 번 호출. */
void audio_codec_init(void);
void audio_codec_deinit(void);

/*
 * pcm_in: AUDIO_CODEC_FRAME_SAMPLES개의 16bit PCM 샘플.
 * out/out_capacity: 인코딩 결과를 담을 버퍼(AUDIO_CODEC_MAX_ENCODED_BYTES 이상 권장).
 * 반환값: 인코딩된 바이트 수, 실패 시 -1.
 */
int audio_codec_encode(const int16_t *pcm_in, uint8_t *out, size_t out_capacity);

/*
 * data/len: 프레임 하나 분량의 인코딩된 데이터.
 * pcm_out: AUDIO_CODEC_FRAME_SAMPLES개의 16bit PCM 샘플을 받을 버퍼.
 * 반환값: 성공 시 0, 실패 시 -1.
 */
int audio_codec_decode(const uint8_t *data, size_t len, int16_t *pcm_out);

#ifdef __cplusplus
}
#endif
