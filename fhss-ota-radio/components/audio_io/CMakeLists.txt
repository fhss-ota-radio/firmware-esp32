# audio_io 컴포넌트: 마이크(INMP441, I2S RX)/스피커(MAX98357A, I2S TX)를 각각
# 별도의 I2S 포트(I2S_NUM_0 / I2S_NUM_1)로 초기화하고, audio_codec의 Speex
# encode/decode와 묶어 capture->encode / decode->play 함수로 제공한다.
#
# REQUIRES는 esp_driver_gpio/esp_driver_i2s로 명시한다 (REQUIRES driver 아님) —
# display_ui/ptt_button과 같은 이유. audio_codec은 audio_io.c 내부 구현에서만
# 쓰이므로(공개 헤더인 audio_io.h/audio_io_config.h는 참조하지 않음) PRIV_REQUIRES.

idf_component_register(
    SRCS "audio_io.c"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_gpio esp_driver_i2s
    PRIV_REQUIRES audio_codec
)
