#include "fhss_audio_pcm_test.h"

#include <limits.h>
#include <string.h>

#include "audio_codec.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "fhss_audio_adapter.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define PCM_TEST_FRAME_PERIOD_MS 20U
#define PCM_TEST_LOG_INTERVAL_FRAMES 50U

static const char *TAG = "fhss_pcm_test";

/* One 32-sample sine lookup cycle. Phase steps 2 and 3 produce 500 Hz and
 * 750 Hz at 8 kHz. Alternating voiced bursts and silence makes RF playback
 * easy to distinguish from random noise without embedding a large file. */
static const int16_t s_sine_lut[32] = {
    0, 1561, 3061, 4445, 5657, 6652, 7391, 7846,
    8000, 7846, 7391, 6652, 5657, 4445, 3061, 1561,
    0, -1561, -3061, -4445, -5657, -6652, -7391, -7846,
    -8000, -7846, -7391, -6652, -5657, -4445, -3061, -1561,
};

static fhss_audio_pcm_test_stats_t s_stats;

static int16_t absolute_sample(int16_t sample)
{
    return sample < 0 ? (int16_t)-sample : sample;
}

static int16_t generate_pcm_frame(uint32_t frame_number, int16_t *pcm)
{
    const uint32_t cycle_frame = frame_number % 100U;
    const bool silence = cycle_frame >= 80U;
    const uint8_t phase_step = cycle_frame < 40U ? 2U : 3U;
    uint8_t phase = (uint8_t)((frame_number * AUDIO_CODEC_FRAME_SAMPLES *
                               phase_step) & 31U);
    int16_t peak = 0;

    for (size_t i = 0U; i < AUDIO_CODEC_FRAME_SAMPLES; ++i) {
        const int16_t sample = silence ? 0 : s_sine_lut[phase];
        pcm[i] = sample;
        const int16_t magnitude = absolute_sample(sample);
        if (magnitude > peak) {
            peak = magnitude;
        }
        phase = (uint8_t)((phase + phase_step) & 31U);
    }
    return peak;
}

void fhss_audio_pcm_test_run(void)
{
    int16_t pcm[AUDIO_CODEC_FRAME_SAMPLES];
    uint8_t encoded[AUDIO_CODEC_MAX_ENCODED_BYTES];
    TickType_t next_wake_tick = xTaskGetTickCount();
    int64_t previous_frame_us = 0;

    memset(&s_stats, 0, sizeof(s_stats));
    s_stats.minimum_interval_us = INT64_MAX;
    ESP_LOGI(TAG,
             "START format=8000Hz mono s16le samples=160 frame_ms=20 "
             "source=500/750Hz-pattern");

    for (;;) {
        const int64_t now_us = esp_timer_get_time();
        if (previous_frame_us != 0) {
            const int64_t interval_us = now_us - previous_frame_us;
            if (interval_us < s_stats.minimum_interval_us) {
                s_stats.minimum_interval_us = interval_us;
            }
            if (interval_us > s_stats.maximum_interval_us) {
                s_stats.maximum_interval_us = interval_us;
            }
        }
        previous_frame_us = now_us;

        const uint32_t frame_number = s_stats.generated_frames;
        const int16_t peak = generate_pcm_frame(frame_number, pcm);
        s_stats.generated_frames++;

        const int encoded_length = audio_codec_encode(
            pcm, encoded, sizeof(encoded));
        if (encoded_length <= 0) {
            s_stats.encode_failures++;
            ESP_LOGE(TAG, "ENCODE_FAIL frame=%lu",
                     (unsigned long)frame_number);
        } else {
            s_stats.encoded_frames++;
            if (fhss_audio_adapter_submit_encoded_frame(
                    encoded, (size_t)encoded_length)) {
                s_stats.submitted_frames++;
            } else {
                s_stats.submit_failures++;
                ESP_LOGW(TAG, "SUBMIT_FAIL frame=%lu encoded_bytes=%d",
                         (unsigned long)frame_number, encoded_length);
            }

            if ((s_stats.generated_frames %
                 PCM_TEST_LOG_INTERVAL_FRAMES) == 0U) {
                ESP_LOGI(TAG,
                         "FRAME frame=%lu pcm_peak=%d encoded_bytes=%d "
                         "submitted=%lu encode_fail=%lu submit_fail=%lu "
                         "interval_us[min/max]=%lld/%lld",
                         (unsigned long)frame_number,
                         peak,
                         encoded_length,
                         (unsigned long)s_stats.submitted_frames,
                         (unsigned long)s_stats.encode_failures,
                         (unsigned long)s_stats.submit_failures,
                         (long long)s_stats.minimum_interval_us,
                         (long long)s_stats.maximum_interval_us);
            }
        }

        /* vTaskDelayUntil preserves the 20 ms cadence instead of adding
         * encoder/log execution time to every frame period. */
        vTaskDelayUntil(
            &next_wake_tick, pdMS_TO_TICKS(PCM_TEST_FRAME_PERIOD_MS));
    }
}

void fhss_audio_pcm_test_get_stats(fhss_audio_pcm_test_stats_t *out_stats)
{
    if (out_stats != NULL) {
        *out_stats = s_stats;
        if (out_stats->minimum_interval_us == INT64_MAX) {
            out_stats->minimum_interval_us = 0;
        }
    }
}
