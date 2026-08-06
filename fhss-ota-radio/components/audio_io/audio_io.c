#include "audio_io.h"

#include "audio_codec.h"

#include "driver/i2s_std.h"
#include "esp_log.h"

static const char *TAG = "audio_io";

static i2s_chan_handle_t s_mic_rx;
static i2s_chan_handle_t s_spk_tx;

static void mic_channel_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(AUDIO_IO_MIC_I2S_PORT, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = AUDIO_IO_I2S_DMA_DESC_NUM;
    chan_cfg.dma_frame_num = AUDIO_IO_I2S_DMA_FRAME_NUM;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &s_mic_rx));

    /* INMP441은 24bit 샘플을 32bit 슬롯에 실어 보낸다 — 마스터(ESP)가 32bit
     * 슬롯 폭으로 BCLK/WS를 생성해야 마이크가 24bit를 다 밀어낼 시간이 나온다.
     * 실제 PCM 변환(상위 16bit 추출)은 audio_io_capture_encode()에서 한다. */
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_CODEC_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = AUDIO_IO_MIC_BCLK_GPIO,
            .ws = AUDIO_IO_MIC_WS_GPIO,
            .dout = I2S_GPIO_UNUSED,
            .din = AUDIO_IO_MIC_SD_GPIO,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_mic_rx, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(s_mic_rx));
}

static void spk_channel_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(AUDIO_IO_SPK_I2S_PORT, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = AUDIO_IO_I2S_DMA_DESC_NUM;
    chan_cfg.dma_frame_num = AUDIO_IO_I2S_DMA_FRAME_NUM;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &s_spk_tx, NULL));

    /* MAX98357A는 16bit PCM을 그대로 흘려보내면 된다 (ESP가 BCLK/WS 마스터라
     * 슬롯 폭 제약이 없음). audio_codec이 이미 16bit PCM을 다루므로 변환 불필요. */
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_CODEC_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = AUDIO_IO_SPK_BCLK_GPIO,
            .ws = AUDIO_IO_SPK_WS_GPIO,
            .dout = AUDIO_IO_SPK_DOUT_GPIO,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_spk_tx, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(s_spk_tx));
}

void audio_io_init(void)
{
    mic_channel_init();
    spk_channel_init();
    ESP_LOGI(TAG, "ready (mic=I2S%d, spk=I2S%d, %dHz/%dsamples)",
              AUDIO_IO_MIC_I2S_PORT, AUDIO_IO_SPK_I2S_PORT,
              AUDIO_CODEC_SAMPLE_RATE, AUDIO_CODEC_FRAME_SAMPLES);
}

int audio_io_capture_encode(uint8_t *out, size_t out_capacity)
{
    int32_t raw[AUDIO_CODEC_FRAME_SAMPLES];
    int16_t pcm[AUDIO_CODEC_FRAME_SAMPLES];

    size_t bytes_read = 0;
    esp_err_t err = i2s_channel_read(s_mic_rx, raw, sizeof(raw), &bytes_read, AUDIO_IO_I2S_TIMEOUT_MS);
    if (err != ESP_OK || bytes_read != sizeof(raw)) {
        ESP_LOGW(TAG, "mic read failed (err=%d, bytes=%u)", err, (unsigned)bytes_read);
        return -1;
    }

    /* INMP441은 32bit 슬롯 안에 24bit 샘플을 MSB 정렬로 담아 보낸다.
     * 상위 16bit만 취해도 음성 대역에는 충분한 해상도의 16bit PCM이 된다. */
    for (int i = 0; i < AUDIO_CODEC_FRAME_SAMPLES; i++) {
        pcm[i] = (int16_t)(raw[i] >> 16);
    }

    return audio_codec_encode(pcm, out, out_capacity);
}

int audio_io_decode_play(const uint8_t *data, size_t len)
{
    int16_t pcm[AUDIO_CODEC_FRAME_SAMPLES];

    if (audio_codec_decode(data, len, pcm) != 0) {
        return -1;
    }

    size_t bytes_written = 0;
    esp_err_t err = i2s_channel_write(s_spk_tx, pcm, sizeof(pcm), &bytes_written, AUDIO_IO_I2S_TIMEOUT_MS);
    if (err != ESP_OK || bytes_written != sizeof(pcm)) {
        ESP_LOGW(TAG, "spk write failed (err=%d, bytes=%u)", err, (unsigned)bytes_written);
        return -1;
    }

    return 0;
}
