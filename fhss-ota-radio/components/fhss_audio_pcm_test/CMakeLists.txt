idf_component_register(
    SRCS "fhss_audio_pcm_test.c"
    INCLUDE_DIRS "include"
    REQUIRES audio_codec fhss_audio_adapter esp_timer freertos log
)
