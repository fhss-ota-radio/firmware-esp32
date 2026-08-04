# display_ui 컴포넌트: 0.96" I2C OLED(SSD1306, 128x64) 상태 표시.
#
# font8x8_basic.h(public domain, dhepper/font8x8)는 컴포넌트 내부 구현
# 디테일이라 공개 include에 노출하지 않는다 — display_ui.c만 참조.

idf_component_register(
    SRCS "display_ui.c"
    INCLUDE_DIRS "."
    REQUIRES driver
)
