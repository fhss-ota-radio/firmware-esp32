#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t generated_frames;
    uint32_t encoded_frames;
    uint32_t submitted_frames;
    uint32_t encode_failures;
    uint32_t submit_failures;
    int64_t minimum_interval_us;
    int64_t maximum_interval_us;
} fhss_audio_pcm_test_stats_t;

/* Runs until the caller deletes its FreeRTOS task. It generates one
 * 8 kHz/mono/signed-16 PCM frame every 20 ms, passes it through the real
 * Speex encoder, and submits the encoded frame to the real FHSS adapter. */
void fhss_audio_pcm_test_run(void);

void fhss_audio_pcm_test_get_stats(fhss_audio_pcm_test_stats_t *out_stats);

#ifdef __cplusplus
}
#endif
