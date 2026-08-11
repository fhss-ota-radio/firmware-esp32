#include "display_ui.h"

#include <stdarg.h>
#include <stdbool.h>
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

static void flush_all_pages(const char *what)
{
    for (uint8_t page = 0; page < DISPLAY_UI_PAGES; page++) {
        if (ssd1306_flush_page(page) != ESP_OK) {
            ESP_LOGE(TAG, "%s: flush page %d failed", what, page);
        }
    }
}

/*
 * === 회전(좌측 90도) 그리기 프리미티브 =============================
 *
 * 배선은 그대로(물리 좌표 px∈[0,DISPLAY_UI_WIDTH), py∈[0,DISPLAY_UI_HEIGHT)),
 * 논리(회전된, 세로) 좌표 lx∈[0,DISPLAY_UI_HEIGHT), ly∈[0,DISPLAY_UI_WIDTH)로
 * 그린다. 매핑(좌측 90도 회전, 실기기로 방향 확인 후 확정 — 처음 식은 반대
 * 방향으로 돌아서 아래로 뒤집음, 2026-08-11):
 *   px = ly
 *   py = (DISPLAY_UI_HEIGHT - 1) - lx
 * 이 아래 함수들은 전부 논리 좌표를 받는다.
 */
static inline void set_pixel(int lx, int ly, bool on)
{
    if (lx < 0 || lx >= DISPLAY_UI_HEIGHT || ly < 0 || ly >= DISPLAY_UI_WIDTH) {
        return;
    }
    int px = ly;
    int py = (DISPLAY_UI_HEIGHT - 1) - lx;
    int page = py / 8;
    int bit = py % 8;
    if (on) {
        s_framebuf[page][px] |= (uint8_t)(1U << bit);
    } else {
        s_framebuf[page][px] &= (uint8_t)~(1U << bit);
    }
}

static void fill_rect(int x, int y, int w, int h, bool on)
{
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            set_pixel(x + i, y + j, on);
        }
    }
}

static void draw_rect_outline(int x, int y, int w, int h, bool on)
{
    for (int i = 0; i < w; i++) {
        set_pixel(x + i, y, on);
        set_pixel(x + i, y + h - 1, on);
    }
    for (int j = 0; j < h; j++) {
        set_pixel(x, y + j, on);
        set_pixel(x + w - 1, y + j, on);
    }
}

/* font8x8_basic 글리프 하나를 scale배 확대해서 (lx,ly)를 좌상단으로 그린다.
 * invert=true면 배경이 이미 채워진 박스 위에 글자를 "깎아내듯"(off) 그린다 —
 * 즉 glyph 비트가 0이든 1이든 pixel_on = !glyph_bit. */
static void draw_glyph_scaled(int lx, int ly, unsigned char c, int scale, bool invert)
{
    const uint8_t *glyph = (c < 128) ? font8x8_basic[c] : font8x8_basic[(unsigned char)' '];

    for (int by = 0; by < 8; by++) {
        for (int bx = 0; bx < 8; bx++) {
            bool glyph_bit = (glyph[by] & (1 << bx)) != 0;
            bool pixel_on = invert ? !glyph_bit : glyph_bit;
            for (int sy = 0; sy < scale; sy++) {
                for (int sx = 0; sx < scale; sx++) {
                    set_pixel(lx + bx * scale + sx, ly + by * scale + sy, pixel_on);
                }
            }
        }
    }
}

/* 왼쪽 정렬로 text를 그린다(scale배 확대). */
static void draw_text(int lx, int ly, const char *text, int scale, bool invert)
{
    for (int i = 0; text[i] != '\0'; i++) {
        draw_glyph_scaled(lx + i * 8 * scale, ly, (unsigned char)text[i], scale, invert);
    }
}

/* box(x,y,w,h) 안에 text를 가로/세로 가운데 정렬로 그린다. */
static void draw_text_centered(int x, int y, int w, int h, const char *text, int scale, bool invert)
{
    int len = (int)strlen(text);
    int text_w = scale * 8 * len;
    int text_h = scale * 8;
    draw_text(x + (w - text_w) / 2, y + (h - text_h) / 2, text, scale, invert);
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
    flush_all_pages("clear");
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

/* 메뉴 화면 레이아웃 상수. 논리 캔버스는 64(=DISPLAY_UI_HEIGHT) x
 * 128(=DISPLAY_UI_WIDTH). 항목 3개(28px) + 간격 2개(8px) = 100px, 헤더
 * 아래(20px)부터 시작해서 끝나면 128 중 8px 남음(여유). */
#define MENU_HEADER_X    4
#define MENU_HEADER_Y    4
#define MENU_ITEM_X      0
#define MENU_ITEM_W      DISPLAY_UI_HEIGHT
#define MENU_ITEM_START_Y 20
#define MENU_ITEM_H      28
#define MENU_ITEM_GAP    8
#define MENU_TEXT_SCALE  2

static const char *const s_menu_labels[DISPLAY_UI_MENU_COUNT] = { "COMM", "IDLE", "OTA" };

void display_ui_draw_menu(display_ui_menu_item_t selected, display_ui_menu_item_t hovered)
{
    memset(s_framebuf, 0, sizeof(s_framebuf));

    draw_text(MENU_HEADER_X, MENU_HEADER_Y, "mode", 1, false);

    for (int i = 0; i < DISPLAY_UI_MENU_COUNT; i++) {
        int item_y = MENU_ITEM_START_Y + i * (MENU_ITEM_H + MENU_ITEM_GAP);
        bool is_selected = (i == selected);
        bool is_hovered = (i == hovered);

        fill_rect(MENU_ITEM_X, item_y, MENU_ITEM_W, MENU_ITEM_H, is_selected);
        draw_text_centered(MENU_ITEM_X, item_y, MENU_ITEM_W, MENU_ITEM_H,
                            s_menu_labels[i], MENU_TEXT_SCALE, is_selected);

        if (is_hovered) {
            /* selected(흰 배경)와 겹치면 테두리는 검은색으로, 아니면(검은
             * 배경) 흰색으로 — 배경과 항상 대비되게. */
            draw_rect_outline(MENU_ITEM_X, item_y, MENU_ITEM_W, MENU_ITEM_H, !is_selected);
        }
    }

    flush_all_pages("draw_menu");
}
