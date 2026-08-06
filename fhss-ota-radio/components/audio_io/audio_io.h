#pragma once

#include <stddef.h>
#include <stdint.h>

#include "audio_io_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 마이크(I2S_NUM_0)/스피커(I2S_NUM_1) 채널 초기화. app_main 등에서 한 번 호출.
 * audio_codec_init()도 별도로 호출해야 한다 — 이 컴포넌트는 I2S 입출력만 담당. */
void audio_io_init(void);

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

#ifdef __cplusplus
}
#endif
