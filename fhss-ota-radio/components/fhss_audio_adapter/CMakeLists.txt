idf_component_register(
    SRCS "fhss_audio_adapter.c"
    INCLUDE_DIRS "include"
    REQUIRES fhss_service fhss_audio_packet audio_codec driver freertos log mbedtls
)
