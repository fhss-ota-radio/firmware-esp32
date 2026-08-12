# esp_efuse_mac_get_default()는 esp_hw_support의 esp_mac.h에 있음
idf_component_register(
    SRCS "device_id.c"
    INCLUDE_DIRS "."
    REQUIRES esp_hw_support
)
