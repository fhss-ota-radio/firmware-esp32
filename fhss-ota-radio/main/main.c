#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "led_strip.h"

// ESP32-S3-DevKitC-1 내장 WS2812 LED. 리비전에 따라 38일 수 있음 (안 켜지면 이 값 변경)
#define BLINK_GPIO 48

static const char *TAG = "blink";
static led_strip_handle_t led_strip;
static bool led_on = false;

static void configure_led(void)
{
    led_strip_config_t strip_config = {
        .strip_gpio_num = BLINK_GPIO,
        .max_leds = 1,
    };
    led_strip_rmt_config_t rmt_config = {
        .resolution_hz = 10 * 1000 * 1000,
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip));
    led_strip_clear(led_strip);
}

void app_main(void)
{
    configure_led();

    while (1) {
        led_on = !led_on;
        if (led_on) {
            led_strip_set_pixel(led_strip, 0, 16, 16, 16);
            led_strip_refresh(led_strip);
        } else {
            led_strip_clear(led_strip);
        }
        ESP_LOGI(TAG, "LED %s", led_on ? "ON" : "OFF");
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}
