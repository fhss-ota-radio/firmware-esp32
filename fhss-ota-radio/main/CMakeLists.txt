idf_component_register(SRCS "main.c" "fsm.c"
                    INCLUDE_DIRS "."
                    REQUIRES display_ui ptt_button rotary_encoder audio_io audio_codec status_led fhss_audio_adapter fhss_audio_pcm_test)
