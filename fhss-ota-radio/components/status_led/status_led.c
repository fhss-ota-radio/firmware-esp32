#include "status_led.h"

#include <stdbool.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "led_strip.h"

static const char *TAG = "status_led";

static led_strip_handle_t s_strip;
static esp_timer_handle_t s_blink_timer;
static bool s_blink_on;

static void blink_tick(void *arg)
{
    s_blink_on = !s_blink_on;
    if (s_blink_on) {
        led_strip_set_pixel(s_strip, 0, STATUS_LED_DIM_BRIGHTNESS, 0, 0);
        led_strip_refresh(s_strip);
    } else {
        led_strip_clear(s_strip);
    }
}

void status_led_init(void)
{
    led_strip_config_t strip_config = {
        .strip_gpio_num = STATUS_LED_GPIO,
        .max_leds = STATUS_LED_MAX_LEDS,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };
    led_strip_rmt_config_t rmt_config = {
        .resolution_hz = 10 * 1000 * 1000, /* 10MHz */
    };

    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &s_strip));
    led_strip_clear(s_strip);

    ESP_LOGI(TAG, "ready (gpio=%d)", STATUS_LED_GPIO);
}

void status_led_set_white_dim(void)
{
    led_strip_set_pixel(s_strip, 0, STATUS_LED_DIM_BRIGHTNESS, STATUS_LED_DIM_BRIGHTNESS, STATUS_LED_DIM_BRIGHTNESS);
    led_strip_refresh(s_strip);
}

void status_led_set_sky_blue_dim(void)
{
    /* 하늘색(#87CEEB) 비율(R:G:B ≈ 135:206:235)을 STATUS_LED_DIM_BRIGHTNESS
     * 기준으로 스케일링. STATUS_LED_DIM_BRIGHTNESS가 작은 값(기본 8)이라
     * 정수 나눗셈으로 R이 0이 되지 않게 최소 1을 보장한다. */
    uint8_t r = (uint8_t)((STATUS_LED_DIM_BRIGHTNESS * 135) / 235);
    uint8_t g = (uint8_t)((STATUS_LED_DIM_BRIGHTNESS * 206) / 235);
    if (STATUS_LED_DIM_BRIGHTNESS > 0 && r == 0) {
        r = 1;
    }
    led_strip_set_pixel(s_strip, 0, r, g, STATUS_LED_DIM_BRIGHTNESS);
    led_strip_refresh(s_strip);
}

void status_led_off(void)
{
    led_strip_clear(s_strip);
}

void status_led_start_error_blink(void)
{
    if (s_blink_timer == NULL) {
        const esp_timer_create_args_t timer_args = {
            .callback = blink_tick,
            .name = "status_led_blink",
        };
        if (esp_timer_create(&timer_args, &s_blink_timer) != ESP_OK) {
            ESP_LOGW(TAG, "blink timer create failed");
            return;
        }
    } else {
        esp_timer_stop(s_blink_timer); /* 재시작 전 정지(이미 돌고 있었을 수 있음) */
    }

    s_blink_on = true;
    led_strip_set_pixel(s_strip, 0, STATUS_LED_DIM_BRIGHTNESS, 0, 0);
    led_strip_refresh(s_strip);
    esp_timer_start_periodic(s_blink_timer, (uint64_t)STATUS_LED_BLINK_INTERVAL_MS * 1000);
}

void status_led_stop_error_blink(void)
{
    if (s_blink_timer != NULL) {
        esp_timer_stop(s_blink_timer); /* 이미 멈춰있어도 안전(에러 무시) */
    }
    led_strip_clear(s_strip);
}
