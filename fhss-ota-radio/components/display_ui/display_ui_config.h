#pragma once

#include "driver/i2c_master.h"
#include "driver/gpio.h"

/*
 * ESP32-S3-DevKitC-1 기준 임의 지정 핀 (부품 배선 확정 전 placeholder).
 * 실제 배선이 정해지면 이 파일만 고치면 된다.
 */
#define DISPLAY_UI_I2C_PORT     I2C_NUM_0
#define DISPLAY_UI_I2C_SDA_GPIO GPIO_NUM_8
#define DISPLAY_UI_I2C_SCL_GPIO GPIO_NUM_9
#define DISPLAY_UI_I2C_FREQ_HZ  400000

/* 0.96" I2C OLED(SSD1306, 128x64) 기본값 */
#define DISPLAY_UI_I2C_ADDR 0x3C
#define DISPLAY_UI_WIDTH    128
#define DISPLAY_UI_HEIGHT   64
#define DISPLAY_UI_PAGES    (DISPLAY_UI_HEIGHT / 8) /* SSD1306 1페이지 = 세로 8px */

/* 8x8 폰트 기준 텍스트 그리드 (1행 = 1페이지) */
#define DISPLAY_UI_TEXT_ROWS DISPLAY_UI_PAGES
#define DISPLAY_UI_TEXT_COLS (DISPLAY_UI_WIDTH / 8)
