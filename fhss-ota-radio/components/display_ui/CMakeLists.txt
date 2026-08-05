# display_ui 컴포넌트: 0.96" I2C OLED(SSD1306, 128x64) 상태 표시.
#
# font8x8_basic.h(public domain, dhepper/font8x8)는 컴포넌트 내부 구현
# 디테일이라 공개 include에 노출하지 않는다 — display_ui.c만 참조.
#
# REQUIRES는 esp_driver_gpio/esp_driver_i2c로 명시한다 (REQUIRES driver 아님).
# ESP-IDF v5.3부터 driver가 esp_driver_* 단위로 쪼개졌고, v6.0부터는 driver의
# 전이 의존성에서 esp_driver_i2c가 빠져 REQUIRES driver만으로는
# driver/i2c_master.h를 못 찾는다 (report: IDF v6.0.2, fatal error 발생).

idf_component_register(
    SRCS "display_ui.c"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_gpio esp_driver_i2c
)
