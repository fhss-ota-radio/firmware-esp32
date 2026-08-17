idf_component_register(SRCS "main.c" "fsm.c"
                    INCLUDE_DIRS "."
                    REQUIRES app_update display_ui ptt_button rotary_encoder audio_io audio_codec status_led device_id ota_client fhss_audio_adapter fhss_audio_pcm_test rf_transport)
