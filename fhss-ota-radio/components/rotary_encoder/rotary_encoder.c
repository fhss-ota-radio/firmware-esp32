#include "rotary_encoder.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "rotary_encoder";

/* A/B 둘 다 내부 풀업 + released=HIGH 가정 시 정지 위치의 raw 값. 배선이 반대(풀다운)면
 * 이 값과 gpio_config()의 pull_up_en 둘 다 맞춰서 바꿔야 한다. */
#define REST_AB 0x3u

static rotary_encoder_cursor_cb_t s_cursor_cb;
static void *s_cursor_ctx;
static rotary_encoder_select_cb_t s_select_cb;
static void *s_select_ctx;

static volatile rotary_encoder_menu_t s_cursor = ROTARY_ENCODER_MENU_COMM;

static TickType_t poll_delay_ticks(void)
{
    const TickType_t ticks = pdMS_TO_TICKS(ROTARY_ENCODER_POLL_MS);
    /* CONFIG_FREERTOS_HZ=100에서는 요청한 2 ms가 0 tick으로 변환된다.
     * vTaskDelay(0) 반복은 IDLE task가 실행될 시간을 보장하지 않아 PTT
     * 폴링 태스크와 함께 CPU를 점유하고 Task Watchdog을 발생시켰다.
     * 최소 1 tick을 사용해 매 폴링마다 CPU를 확실히 양보한다. */
    return ticks > 0U ? ticks : 1U;
}

/*
 * raw AB(2bit, bit0=A, bit1=B) -> gray code 순번(0~3).
 * 한 방향 회전 시 raw 값은 00->01->11->10->00 순서로 매 스텝 1비트씩만 바뀐다
 * (정상적인 quadrature 신호의 정의) — 이 순서에서의 위치를 순번화한 표.
 * raw: 0(00)->순번0, 1(01)->순번1, 3(11)->순번2, 2(10)->순번3
 */
static const int8_t s_gray_to_step[4] = {0, 1, 3, 2};

static inline uint8_t read_ab(void)
{
    int a = gpio_get_level(ROTARY_ENCODER_GPIO_A);
    int b = gpio_get_level(ROTARY_ENCODER_GPIO_B);
    return (uint8_t)((b << 1) | a);
}

static inline bool sw_raw_pressed(void)
{
    int level = gpio_get_level(ROTARY_ENCODER_GPIO_SW);
#if ROTARY_ENCODER_SW_ACTIVE_LOW
    return level == 0;
#else
    return level != 0;
#endif
}

static void move_cursor(int delta_detents)
{
    int next = ((int)s_cursor + delta_detents) % ROTARY_ENCODER_MENU_COUNT;
    if (next < 0) {
        next += ROTARY_ENCODER_MENU_COUNT;
    }
    if ((rotary_encoder_menu_t)next == s_cursor) {
        return;
    }
    s_cursor = (rotary_encoder_menu_t)next;
    ESP_LOGI(TAG, "cursor -> %d", (int)s_cursor);
    if (s_cursor_cb != NULL) {
        s_cursor_cb(s_cursor, s_cursor_ctx);
    }
}

static void rotary_encoder_task(void *arg)
{
    const TickType_t delay_ticks = poll_delay_ticks();
    uint8_t prev_ab = read_ab();
    int accum = 0;

    bool sw_candidate = sw_raw_pressed();
    int sw_stable_count = ROTARY_ENCODER_SW_DEBOUNCE_COUNT;
    bool sw_pressed = sw_candidate;

    for (;;) {
        /* --- 회전: A/B quadrature 디코딩 --- */
        uint8_t curr_ab = read_ab();
        if (curr_ab != prev_ab) {
            int8_t prev_step = s_gray_to_step[prev_ab];
            int8_t curr_step = s_gray_to_step[curr_ab];
            int8_t delta = (int8_t)(((curr_step - prev_step) + 4) % 4);
            /* delta==1/3 중 어느 쪽이 실제 시계방향인지는 A/B 배선 순서에
             * 달려있어서 코드만으로 알 수 없다 — 실기기로 확인해보니 반대라
             * 여기서 뒤집어서 물리적 시계방향이 accum++(메뉴 아래로)가
             * 되게 맞춤(2026-08-11). 방향이 또 바뀌면 이 두 줄만 서로
             * 바꾸면 된다(README의 "A/B 핀 서로 바꾸기"와 동일 효과). */
            if (delta == 3) {
                accum++;   /* 물리적 시계방향 1 step */
            } else if (delta == 1) {
                accum--;   /* 물리적 반시계방향 1 step */
            }
            /* delta==2: 두 스텝 이상 건너뜀(폴링 놓침/노이즈) — 신뢰 못 하므로 무시 */
            prev_ab = curr_ab;
        }

        /* 정지 위치로 돌아왔을 때만 detent를 확정한다. 중간에서 멈추면 다음에
         * 정지 위치로 돌아올 때까지 accum을 들고 대기한다. */
        if (curr_ab == REST_AB && accum != 0) {
            if (accum >= ROTARY_ENCODER_STEPS_PER_DETENT) {
                move_cursor(+1); /* 시계방향 = 메뉴 아래로 (COMM -> IDLE -> OTA -> COMM) */
            } else if (accum <= -ROTARY_ENCODER_STEPS_PER_DETENT) {
                move_cursor(-1);
            }
            accum = 0;
        }

        /* --- SW(클릭) 디바운스: ptt_button과 동일한 폴링 방식 --- */
        bool sw_level = sw_raw_pressed();
        if (sw_level == sw_candidate) {
            if (sw_stable_count < ROTARY_ENCODER_SW_DEBOUNCE_COUNT) {
                sw_stable_count++;
            }
        } else {
            sw_candidate = sw_level;
            sw_stable_count = 1;
        }

        if (sw_stable_count == ROTARY_ENCODER_SW_DEBOUNCE_COUNT && sw_candidate != sw_pressed) {
            sw_pressed = sw_candidate;
            if (sw_pressed) {
                ESP_LOGI(TAG, "select confirmed: %d", (int)s_cursor);
                if (s_select_cb != NULL) {
                    s_select_cb(s_cursor, s_select_ctx);
                }
            }
        }

        vTaskDelay(delay_ticks);
    }
}

void rotary_encoder_init(void)
{
    gpio_config_t ab_cfg = {
        .pin_bit_mask = (1ULL << ROTARY_ENCODER_GPIO_A) | (1ULL << ROTARY_ENCODER_GPIO_B),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&ab_cfg);

    gpio_config_t sw_cfg = {
        .pin_bit_mask = 1ULL << ROTARY_ENCODER_GPIO_SW,
        .mode = GPIO_MODE_INPUT,
#if ROTARY_ENCODER_SW_ACTIVE_LOW
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
#else
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
#endif
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&sw_cfg);

    xTaskCreate(rotary_encoder_task, "rotary_encoder", ROTARY_ENCODER_TASK_STACK, NULL,
                ROTARY_ENCODER_TASK_PRIORITY, NULL);

    ESP_LOGI(TAG, "ready (A=%d, B=%d, SW=%d, poll=%lums)",
             ROTARY_ENCODER_GPIO_A,
             ROTARY_ENCODER_GPIO_B,
             ROTARY_ENCODER_GPIO_SW,
             (unsigned long)(poll_delay_ticks() * portTICK_PERIOD_MS));
}

void rotary_encoder_set_cursor_callback(rotary_encoder_cursor_cb_t cb, void *ctx)
{
    s_cursor_cb = cb;
    s_cursor_ctx = ctx;
}

void rotary_encoder_set_select_callback(rotary_encoder_select_cb_t cb, void *ctx)
{
    s_select_cb = cb;
    s_select_ctx = ctx;
}

rotary_encoder_menu_t rotary_encoder_get_cursor(void)
{
    return s_cursor;
}
