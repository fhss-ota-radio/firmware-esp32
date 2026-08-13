#include "ptt_button.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "ptt_button";

static ptt_button_cb_t s_cb;
static void *s_cb_ctx;
static volatile bool s_pressed;

static TickType_t poll_delay_ticks(void)
{
    const TickType_t ticks = pdMS_TO_TICKS(PTT_BUTTON_POLL_MS);
    /* CONFIG_FREERTOS_HZ=100에서는 1 tick이 10 ms라서 요청한 5 ms가
     * 0 tick으로 잘린다. vTaskDelay(0)는 CPU를 확실히 양보하지 않아 이
     * 폴링 태스크가 IDLE task를 굶기고 Task Watchdog을 발생시켰다.
     * 최소 1 tick을 보장해 입력 폴링 사이에 반드시 스케줄링 기회를 준다. */
    return ticks > 0U ? ticks : 1U;
}

static inline bool raw_level_pressed(void)
{
    int level = gpio_get_level(PTT_BUTTON_GPIO);
#if PTT_BUTTON_ACTIVE_LOW
    return level == 0;
#else
    return level != 0;
#endif
}

/*
 * 인터럽트 대신 폴링 + "연속 N회 안정" 방식으로 디바운스한다. ISR/esp_timer
 * 조합보다 단순하고, 이 정도 응답속도(수 ms)면 PTT 용도로 충분하다.
 */
static void ptt_button_task(void *arg)
{
    const TickType_t delay_ticks = poll_delay_ticks();
    bool candidate = raw_level_pressed();
    int stable_count = PTT_BUTTON_DEBOUNCE_COUNT;
    s_pressed = candidate;

    for (;;) {
        bool level = raw_level_pressed();

        if (level == candidate) {
            if (stable_count < PTT_BUTTON_DEBOUNCE_COUNT) {
                stable_count++;
            }
        } else {
            candidate = level;
            stable_count = 1;
        }

        if (stable_count == PTT_BUTTON_DEBOUNCE_COUNT && candidate != s_pressed) {
            s_pressed = candidate;
            ESP_LOGI(TAG, "%s", s_pressed ? "pressed" : "released");
            if (s_cb != NULL) {
                s_cb(s_pressed, s_cb_ctx);
            }
        }

        vTaskDelay(delay_ticks);
    }
}

void ptt_button_init(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << PTT_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
#if PTT_BUTTON_ACTIVE_LOW
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
#else
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
#endif
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);

    xTaskCreate(ptt_button_task, "ptt_button", PTT_BUTTON_TASK_STACK, NULL,
                PTT_BUTTON_TASK_PRIORITY, NULL);

    ESP_LOGI(TAG,
             "ready (gpio=%d, active_%s, poll=%lums, debounce=%lums)",
             PTT_BUTTON_GPIO,
             PTT_BUTTON_ACTIVE_LOW ? "low" : "high",
             (unsigned long)(poll_delay_ticks() * portTICK_PERIOD_MS),
             (unsigned long)(poll_delay_ticks() * portTICK_PERIOD_MS *
                             PTT_BUTTON_DEBOUNCE_COUNT));
}

void ptt_button_set_callback(ptt_button_cb_t cb, void *ctx)
{
    s_cb = cb;
    s_cb_ctx = ctx;
}

bool ptt_button_is_pressed(void)
{
    return s_pressed;
}
