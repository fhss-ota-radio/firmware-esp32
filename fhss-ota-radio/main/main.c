#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#include "audio_codec.h"

static const char *TAG = "main";

void app_main(void)
{
    audio_codec_init();

    /* 스모크 테스트: 무음 프레임을 인코딩 후 곧바로 디코딩해 연결 확인. */
    int16_t pcm_in[AUDIO_CODEC_FRAME_SAMPLES];
    memset(pcm_in, 0, sizeof(pcm_in));

    uint8_t encoded[AUDIO_CODEC_MAX_ENCODED_BYTES];
    int encoded_len = audio_codec_encode(pcm_in, encoded, sizeof(encoded));
    if (encoded_len <= 0) {
        ESP_LOGE(TAG, "codec smoke test: encode failed");
        return;
    }
    ESP_LOGI(TAG, "codec smoke test: encoded %d bytes", encoded_len);

    int16_t pcm_out[AUDIO_CODEC_FRAME_SAMPLES];
    if (audio_codec_decode(encoded, (size_t)encoded_len, pcm_out) != 0) {
        ESP_LOGE(TAG, "codec smoke test: decode failed");
        return;
    }
    ESP_LOGI(TAG, "codec smoke test: decode ok");
}
