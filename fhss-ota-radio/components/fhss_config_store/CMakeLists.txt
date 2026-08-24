idf_component_register(
    SRCS "fhss_config_store.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash ota_protocol
)
