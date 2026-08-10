idf_component_register(
    SRCS
        "main.c" "fsm.c"
        "ota_test/ota_test_task.c"

    INCLUDE_DIRS
        "." "ota_test"

    REQUIRES
        display_ui ptt_button rotary_encoder audio_io audio_codec status_led
        ota_client app_update esp_partition bootloader_support
)

if(NOT DEFINED OTA_TEST_BUILD_ID)
    set(OTA_TEST_BUILD_ID "base")
endif()

target_compile_definitions(
    ${COMPONENT_LIB}
    PRIVATE OTA_TEST_BUILD_ID="${OTA_TEST_BUILD_ID}"
)
