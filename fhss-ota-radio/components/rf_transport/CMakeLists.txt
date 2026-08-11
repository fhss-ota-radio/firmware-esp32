idf_component_register(
    SRCS
        "rf_transport.c"

    INCLUDE_DIRS
        "include"

    REQUIRES
        esp_driver_spi
        esp_driver_gpio

    PRIV_REQUIRES
        esp_timer
        freertos
)
