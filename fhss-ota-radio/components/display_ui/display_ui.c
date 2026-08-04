#include "display_ui.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#include "font8x8_basic.h"

static const char *TAG = "display_ui";

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static uint8_t s_framebuf[DISPLAY_UI_PAGES][DISPLAY_UI_WIDTH];

/* 표준 SSD1306 128x64 init 시퀀스 (page addressing mode, charge pump on). */
static const uint8_t s_init_cmds[] = {
    0xAE,             /* display off */
    0x20, 0x02,       /* memory addressing mode = page addressing */
    0xB0,             /* page start address 0 */
    0xC8,             /* COM output scan direction: remapped */
    0x00,             /* column address low nibble = 0 */
    0x10,             /* column address high nibble = 0 */
    0x40,             /* display start line = 0 */
    0x81, 0x7F,       /* contrast control */
    0xA1,             /* segment re-map */
    0xA6,             /* normal (non-inverted) display */
    0xA8, 0x3F,       /* multiplex ratio = 63 (64 rows) */
    0xA4,             /* entire display follows RAM content */
    0xD3, 0x00,       /* display offset = 0 */
    0xD5, 0x80,       /* display clock divide ratio / osc freq */
    0xD9, 0xF1,       /* pre-charge period */
    0xDA, 0x12,       /* COM pins hardware config (128x64) */
    0xDB, 0x40,       /* VCOMH deselect level */
    0x8D, 0x14,       /* charge pump enable */
    0xAF,             /* display on */
};

/* control_byte: 0x00=command 스트림, 0x40=데이터(GDDRAM) 스트림. */
static esp_err_t ssd1306_write(uint8_t control_byte, const uint8_t *data, size_t len)
{
    uint8_t buf[1 + DISPLAY_UI_WIDTH];
    if (len > sizeof(buf) - 1) {
        return ESP_ERR_INVALID_SIZE;
    }
    buf[0] = control_byte;
    memcpy(&buf[1], data, len);
    return i2c_master_transmit(s_dev, buf, len + 1, -1);
}

static esp_err_t ssd1306_flush_page(uint8_t page)
{
    const uint8_t set_page_cmds[] = {
        (uint8_t)(0xB0 | (page & 0x0F)), /* page start address */
        0x00,                             /* column low nibble = 0 */
        0x10,                             /* column high nibble = 0 */
    };
    esp_err_t err = ssd1306_write(0x00, set_page_cmds, sizeof(set_page_cmds));
    if (err != ESP_OK) {
        return err;
    }
    return ssd1306_write(0x40, s_framebuf[page], DISPLAY_UI_WIDTH);
}

void display_ui_init(void)
{
    i2c_master_bus_config_t bus_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = DISPLAY_UI_I2C_PORT,
        .scl_io_num = DISPLAY_UI_I2C_SCL_GPIO,
        .sda_io_num = DISPLAY_UI_I2C_SDA_GPIO,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    if (i2c_new_master_bus(&bus_config, &s_bus) != ESP_OK) {
        ESP_LOGE(TAG, "i2c bus init failed");
        return;
    }

    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = DISPLAY_UI_I2C_ADDR,
        .scl_speed_hz = DISPLAY_UI_I2C_FREQ_HZ,
    };
    if (i2c_master_bus_add_device(s_bus, &dev_config, &s_dev) != ESP_OK) {
        ESP_LOGE(TAG, "i2c device add failed");
        return;
    }

    if (ssd1306_write(0x00, s_init_cmds, sizeof(s_init_cmds)) != ESP_OK) {
        ESP_LOGE(TAG, "ssd1306 init sequence failed");
        return;
    }

    display_ui_clear();
    ESP_LOGI(TAG, "ssd1306 ready (%dx%d, addr 0x%02X)",
             DISPLAY_UI_WIDTH, DISPLAY_UI_HEIGHT, DISPLAY_UI_I2C_ADDR);
}

void display_ui_clear(void)
{
    memset(s_framebuf, 0, sizeof(s_framebuf));
    for (uint8_t page = 0; page < DISPLAY_UI_PAGES; page++) {
        if (ssd1306_flush_page(page) != ESP_OK) {
            ESP_LOGE(TAG, "clear: flush page %d failed", page);
        }
    }
}

void oled_update_text(uint8_t row, const char *text)
{
    if (row >= DISPLAY_UI_TEXT_ROWS || text == NULL) {
        return;
    }

    uint8_t *page = s_framebuf[row];
    memset(page, 0, DISPLAY_UI_WIDTH);

    size_t col = 0;
    for (size_t i = 0; text[i] != '\0' && col < DISPLAY_UI_TEXT_COLS; i++, col++) {
        unsigned char c = (unsigned char)text[i];
        const uint8_t *glyph = (c < 128) ? font8x8_basic[c] : font8x8_basic[(unsigned char)' '];

        /* font8x8_basic[glyph][row]는 바이트 하나가 그 행의 가로 8픽셀을 나타내는
         * row-major 포맷이다. SSD1306 페이지 메모리는 바이트 하나가 세로 8픽셀
         * (컬럼)을 나타내는 column-major라 여기서 전치(transpose)한다. */
        for (int bx = 0; bx < 8; bx++) {
            uint8_t column_byte = 0;
            for (int by = 0; by < 8; by++) {
                if (glyph[by] & (1 << bx)) {
                    column_byte |= (1 << by);
                }
            }
            page[col * 8 + bx] = column_byte;
        }
    }

    if (ssd1306_flush_page(row) != ESP_OK) {
        ESP_LOGE(TAG, "update row %d failed", row);
    }
}

void oled_update_text_fmt(uint8_t row, const char *fmt, ...)
{
    char buf[DISPLAY_UI_TEXT_COLS + 1];

    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    oled_update_text(row, buf);
}
