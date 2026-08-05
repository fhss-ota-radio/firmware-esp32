# REQUIRES는 esp_driver_gpio로 명시한다 (REQUIRES driver 아님) — display_ui와
# 같은 이유. ESP-IDF v6.0부터 driver의 전이 의존성만으로는 gpio 서브컴포넌트가
# 보장되지 않는다.
idf_component_register(
    SRCS "ptt_button.c"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_gpio
)
