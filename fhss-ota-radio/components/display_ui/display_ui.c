#include "display_ui.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"

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

/*
 * 메뉴 화면 레이아웃 상수. 논리 캔버스는 64(=DISPLAY_UI_HEIGHT, 가로) x
 * 128(=DISPLAY_UI_WIDTH, 세로). 항목 3개(24px) + 간격 2개(6px) = 84px,
 * 헤더 아래(16px)부터 시작해서 끝나면(100px 지점) 상태 메시지 영역으로
 * STATUS_H(28px)를 확보한다(2026-08-12 — 예전엔 항목이 더 커서(28px/8px
 * 간격) 상태 표시용 여유가 8px밖에 안 남았음, 지금 모드별 상태 텍스트를
 * 넣으려고 항목을 살짝 줄임).
 */
#define MENU_HEADER_X    4
#define MENU_HEADER_Y    4
#define MENU_ITEM_X      0
#define MENU_ITEM_W      DISPLAY_UI_HEIGHT
#define MENU_ITEM_START_Y 16
#define MENU_ITEM_H      24
#define MENU_ITEM_GAP    6
#define MENU_TEXT_SCALE  2

/*
 * 상태 메시지 영역(메뉴 3항목 바로 아래, 화면 끝까지). scale1 폰트라
 * 한 글자 8px, 논리 너비 64px이므로 한 줄에 STATUS_MAX_CHARS(8자)까지만
 * 들어간다 — 더 긴 문구는 짧게 줄여서 넣을 것. 애니메이션 문구(base+마침표
 * 0~3개)는 base가 5자 이하여야 마침표 3개를 더해도 8자를 안 넘는다.
 * STATUS_H(28px)는 텍스트 한 줄(8px)보다 넉넉히 잡아뒀다 — 나중에 OTA
 * 진행률 바를 텍스트 아래에 추가할 여유 공간(TODO, 팀2).
 */
#define STATUS_MAX_CHARS  8
#define STATUS_DOT_MAX    3
/* s_status_text 버퍼는 base(최대 STATUS_MAX_CHARS자) + 마침표(최대
 * STATUS_DOT_MAX개) + NUL의 이론상 최댓값으로 잡는다 — 실제 화면엔
 * STATUS_MAX_CHARS자까지만 보이지만(위 주석 참고), 버퍼를 이렇게 넉넉히
 * 잡아둬야 컴파일러가 snprintf()의 "%s%.*s" 결합에서 트렁케이션 가능성을
 * 정적으로 배제할 수 있다(안 그러면 -Werror=format-truncation 빌드 에러). */
#define STATUS_TEXT_BUF_LEN (STATUS_MAX_CHARS + STATUS_DOT_MAX + 1)
#define STATUS_TEXT_SCALE 1
#define STATUS_Y (MENU_ITEM_START_Y + DISPLAY_UI_MENU_COUNT * MENU_ITEM_H \
                  + (DISPLAY_UI_MENU_COUNT - 1) * MENU_ITEM_GAP)
#define STATUS_H (DISPLAY_UI_WIDTH - STATUS_Y)
#define STATUS_ANIM_INTERVAL_MS 250

static const char *const s_menu_labels[DISPLAY_UI_MENU_COUNT] = { "COMM", "IDLE", "OTA" };

/* display_ui_draw_menu()/display_ui_set_status*()가 공유하는 현재 화면 상태.
 * 각 API는 이 중 자기 관련 필드만 갱신하고 항상 render_screen()으로 전체를
 * 다시 그린다 — 그래야 예를 들어 상태 텍스트만 바뀌어도(display_ui_set_status)
 * 마지막으로 그렸던 메뉴 선택/커서가 화면에 계속 유지된다. */
static display_ui_menu_item_t s_last_selected = DISPLAY_UI_MENU_COMM;
static display_ui_menu_item_t s_last_hovered = DISPLAY_UI_MENU_COMM;
static char s_status_base[STATUS_MAX_CHARS + 1];
static char s_status_text[STATUS_TEXT_BUF_LEN];
static int s_status_dot_count;
static esp_timer_handle_t s_status_timer;

static void render_screen(void)
{
    memset(s_framebuf, 0, sizeof(s_framebuf));

    draw_text(MENU_HEADER_X, MENU_HEADER_Y, "mode", 1, false);

    for (int i = 0; i < DISPLAY_UI_MENU_COUNT; i++) {
        int item_y = MENU_ITEM_START_Y + i * (MENU_ITEM_H + MENU_ITEM_GAP);
        bool is_selected = (i == s_last_selected);
        bool is_hovered = (i == s_last_hovered);

        fill_rect(MENU_ITEM_X, item_y, MENU_ITEM_W, MENU_ITEM_H, is_selected);
        draw_text_centered(MENU_ITEM_X, item_y, MENU_ITEM_W, MENU_ITEM_H,
                            s_menu_labels[i], MENU_TEXT_SCALE, is_selected);

        if (is_hovered) {
            /* selected(흰 배경)와 겹치면 테두리는 검은색으로, 아니면(검은
             * 배경) 흰색으로 — 배경과 항상 대비되게. */
            draw_rect_outline(MENU_ITEM_X, item_y, MENU_ITEM_W, MENU_ITEM_H, !is_selected);
        }
    }

    if (s_status_text[0] != '\0') {
        draw_text_centered(MENU_ITEM_X, STATUS_Y, MENU_ITEM_W, STATUS_H,
                            s_status_text, STATUS_TEXT_SCALE, false);
    }

    flush_all_pages("render_screen");
}

/* 애니메이션 타이머가 도는 중이면 멈춘다. NULL(한 번도 애니메이션을 시작한
 * 적 없음)이면 아무 것도 안 함 — esp_timer_stop(NULL) 크래시 방지. */
static void stop_status_animation(void)
{
    if (s_status_timer != NULL) {
        esp_timer_stop(s_status_timer); /* 이미 멈춰있어도 안전(에러 무시) */
    }
}

/* esp_timer 콜백은 esp_timer 전용 태스크 컨텍스트에서 250ms마다 불린다.
 * fsm_task/rotary_encoder_task도 각자 display_ui_draw_menu()/set_status*()를
 * 부를 수 있어 s_framebuf 등 공유 상태에 뮤텍스 없이 여러 태스크가 접근하는
 * 구조인데, 이건 이 컴포넌트가 원래(회전 그리기 도입 때부터) 갖고 있던
 * 패턴을 그대로 따른 것 — 갱신이 드물고(사람이 보는 UI) 한 프레임 정도
 * 밀려도 다음 tick에서 바로 정정되니 지금은 문제되지 않는다. */
static void status_anim_tick(void *arg)
{
    s_status_dot_count = (s_status_dot_count + 1) % (STATUS_DOT_MAX + 1);
    snprintf(s_status_text, sizeof(s_status_text), "%s%.*s", s_status_base, s_status_dot_count, "...");
    render_screen();
}

void display_ui_draw_menu(display_ui_menu_item_t selected, display_ui_menu_item_t hovered)
{
    s_last_selected = selected;
    s_last_hovered = hovered;
    render_screen();
}

void display_ui_set_status(const char *text)
{
    stop_status_animation();
    snprintf(s_status_text, sizeof(s_status_text), "%s", (text != NULL) ? text : "");
    render_screen();
}

void display_ui_set_status_animated(const char *base)
{
    snprintf(s_status_base, sizeof(s_status_base), "%s", (base != NULL) ? base : "");
    s_status_dot_count = 0;
    snprintf(s_status_text, sizeof(s_status_text), "%s", s_status_base);

    if (s_status_timer == NULL) {
        const esp_timer_create_args_t timer_args = {
            .callback = status_anim_tick,
            .name = "status_anim",
        };
        if (esp_timer_create(&timer_args, &s_status_timer) != ESP_OK) {
            ESP_LOGW(TAG, "status anim timer create failed");
            render_screen();
            return;
        }
    } else {
        esp_timer_stop(s_status_timer); /* 재시작 전 정지(이미 돌고 있었을 수 있음) */
    }

    esp_timer_start_periodic(s_status_timer, (uint64_t)STATUS_ANIM_INTERVAL_MS * 1000);
    render_screen();
}

void display_ui_clear_status(void)
{
    stop_status_animation();
    s_status_text[0] = '\0';
    render_screen();
}
