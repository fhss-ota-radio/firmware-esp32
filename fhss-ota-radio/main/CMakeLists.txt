idf_component_register(
    SRCS
        "main.c" "fsm.c"
        "ota_test/ota_test_task.c"

    INCLUDE_DIRS
        "." "ota_test"

    REQUIRES
        display_ui ptt_button rotary_encoder audio_io audio_codec status_led
        ota_client
)
