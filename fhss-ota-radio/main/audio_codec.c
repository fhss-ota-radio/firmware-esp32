#include "audio_codec.h"

#include "esp_log.h"

#include "speex/speex.h"
#include "speex/speex_bits.h"

static const char *TAG = "audio_codec";

static void *s_enc_state;
static void *s_dec_state;
static SpeexBits s_enc_bits;
static SpeexBits s_dec_bits;

void audio_codec_init(void)
{
    /* 0~10, 값이 클수록 고음질/고비트레이트. 무전기 대역폭 절충 기본값. */
    int quality = 4;

    s_enc_state = speex_encoder_init(&speex_nb_mode);
    speex_encoder_ctl(s_enc_state, SPEEX_SET_QUALITY, &quality);
    speex_bits_init(&s_enc_bits);

    s_dec_state = speex_decoder_init(&speex_nb_mode);
    speex_bits_init(&s_dec_bits);

    ESP_LOGI(TAG, "speex narrowband ready (%d Hz, %d samples/frame, quality=%d)",
             AUDIO_CODEC_SAMPLE_RATE, AUDIO_CODEC_FRAME_SAMPLES, quality);
}

void audio_codec_deinit(void)
{
    speex_bits_destroy(&s_enc_bits);
    speex_bits_destroy(&s_dec_bits);
    speex_encoder_destroy(s_enc_state);
    speex_decoder_destroy(s_dec_state);
}

int audio_codec_encode(const int16_t *pcm_in, uint8_t *out, size_t out_capacity)
{
    speex_bits_reset(&s_enc_bits);
    speex_encode_int(s_enc_state, (spx_int16_t *)pcm_in, &s_enc_bits);

    int packed = speex_bits_write(&s_enc_bits, (char *)out, (int)out_capacity);
    if (packed <= 0) {
        ESP_LOGE(TAG, "encode failed");
        return -1;
    }
    return packed;
}

int audio_codec_decode(const uint8_t *data, size_t len, int16_t *pcm_out)
{
    speex_bits_read_from(&s_dec_bits, (const char *)data, (int)len);
    if (speex_decode_int(s_dec_state, &s_dec_bits, (spx_int16_t *)pcm_out) != 0) {
        ESP_LOGE(TAG, "decode failed");
        return -1;
    }
    return 0;
}
