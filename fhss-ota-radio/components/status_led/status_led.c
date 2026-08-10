#include "status_led.h"

#include "esp_log.h"
#include "led_strip.h"

static const char *TAG = "status_led";

static led_strip_handle_t s_strip;

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

void status_led_off(void)
{
    led_strip_clear(s_strip);
}
