idf_component_register(
    SRCS "fhss_audio_adapter.c"
    INCLUDE_DIRS "include"
    REQUIRES fhss_service fhss_audio_packet fhss_config_store audio_codec ota_protocol driver freertos log
)
