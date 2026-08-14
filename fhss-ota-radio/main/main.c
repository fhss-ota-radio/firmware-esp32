#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "fsm.h"
#include "rf_transport.h"

/* TEMP(CC1101 단독 진단): 통합 초기화에서 OLED/I2C, 오디오, FSM이 함께
 * 시작되는 영향을 배제하고 과거 smoke test와 같은 최소 SPI 경로만 실행한다.
 * CC1101 통신 원인이 확인되면 0으로 바꿔 기존 app_main 경로를 복원한다. */
#define CC1101_STANDALONE_DIAGNOSTIC 1

#if CC1101_STANDALONE_DIAGNOSTIC
/* 재배선(2026-08-14): 앰프와의 간섭으로 CC1101도 이동. 특히 GPIO14는 기존
 * CS 자리였는데 보드 하드웨어 배치상 GND와 물리적으로 묶일 수밖에 없어서,
 * 여기서 CS로도, 다른 어떤 GPIO 용도로도 절대 쓰면 안 된다(출력으로 잡고
 * HIGH를 내보내면 GND와 직결 쇼트) — CS는 GPIO10으로 옮겨서 회피. */
#define CC1101_DIAG_SCLK_GPIO GPIO_NUM_12
#define CC1101_DIAG_MOSI_GPIO GPIO_NUM_9
#define CC1101_DIAG_MISO_GPIO GPIO_NUM_11
#define CC1101_DIAG_CS_GPIO   GPIO_NUM_10

static void cc1101_standalone_diagnostic(void)
{
    static rf_transport_t transport;
    const rf_transport_config_t config = {
        .spi_host = SPI2_HOST,
        .sclk_gpio = CC1101_DIAG_SCLK_GPIO,
        .mosi_gpio = CC1101_DIAG_MOSI_GPIO,
        .miso_gpio = CC1101_DIAG_MISO_GPIO,
        .cs_gpio = CC1101_DIAG_CS_GPIO,
        .gdo0_gpio = GPIO_NUM_NC,
        .spi_clock_hz = 1000000,
        /* 칩 식별/read-back에는 GDO0가 필요하지 않아 ISR 영향을 배제한다. */
        .enable_gdo0_interrupt = false,
    };

    ESP_LOGW("cc1101_diag",
             "CC1101 mode: SCLK=%d MOSI=%d MISO=%d CS=%d; FSM/OLED/audio skipped",
             config.sclk_gpio, config.mosi_gpio,
             config.miso_gpio, config.cs_gpio);

    rf_transport_status_t status = rf_transport_init(&transport, &config);
    ESP_LOGI("cc1101_diag", "init status=%d", status);
    if (status != RF_TRANSPORT_STATUS_OK) {
        return;
    }

    rf_transport_chip_info_t info = {0};
    status = rf_transport_read_chip_info(&transport, &info);
    ESP_LOGI("cc1101_diag",
             "pre-config status=%d PARTNUM=0x%02X VERSION=0x%02X MISO_LEVEL=%d",
             status, info.partnum, info.version,
             gpio_get_level(CC1101_DIAG_MISO_GPIO));

    status = rf_transport_configure_433mhz(&transport);
    ESP_LOGI("cc1101_diag", "configure/read-back status=%d MISO_LEVEL=%d",
             status, gpio_get_level(CC1101_DIAG_MISO_GPIO));
}
#endif

/*
 * TEMP(진단용, 원인 확인 끝나면 이 블록 전부 제거): task_wdt가 "IDLE1이 5초
 * 동안 안 돌았다"고 계속 경고하는데, 그 시점에 "마침 실행 중이던" 태스크
 * (ptt_button/rotary_encoder)가 로그에 찍힌다고 해서 그게 실제 CPU를 많이
 * 쓰는 범인이라는 뜻은 아니다(워치독은 체크 시점의 스냅샷만 찍음). 태스크별
 * 실제 CPU 점유율을 sdkconfig에서 CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS로
 * 켠 런타임 통계로 직접 확인한다.
 *
 * CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS는 sdkconfig(git 비추적, 로컬
 * 전용) 안에 있어서 이 커밋만으론 다른 사람 로컬에 안 퍼진다 — 그 옵션이
 * 꺼진 sdkconfig에서는 vTaskGetRunTimeStats() 선언 자체가 FreeRTOS
 * task.h에서 빠져서 TASK_STATS_LOG_ENABLE만 보고 무조건 컴파일하면 빌드가
 * 깨진다. 그래서 실제 Kconfig 매크로도 같이 확인해서, 옵션이 꺼진 로컬
 * 환경에서는 이 블록 전체가 조용히 빠지게 한다(빌드는 되지만 로그는 안 찍힘
 * — 그럴 땐 sdkconfig에서 해당 옵션을 켜야 함). */
#define TASK_STATS_LOG_ENABLE
#if defined(TASK_STATS_LOG_ENABLE) && CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
#define TASK_STATS_BUF_LEN     1024
#define TASK_STATS_INTERVAL_MS 2000
/* idle+1로는 이 태스크 자신도 스케줄링을 못 받는 게 실측으로 확인됨(부팅
 * 직후 1회 찍고 그 뒤로 계속 굶음) — 앱 태스크가 전부 idle+3이라 그보다
 * 높여야 진짜 범인이 무엇이든 끼어들어서 통계를 뽑아낼 수 있다. */
#define TASK_STATS_PRIORITY (tskIDLE_PRIORITY + 10)

static void task_stats_task(void *arg)
{
    /* 대기 없이 시작하자마자 한 줄 찍는다 — TASK_STATS_PRIORITY까지 올려도
     * 이 로그조차 다시 안 보이면, 앱 태스크보다도 훨씬 높은 우선순위의
     * 무언가(또는 인터럽트 컨텍스트)가 계속 CPU를 붙잡고 있다는 뜻. */
    ESP_LOGI("task_stats", "task_stats_task started on core %d", xPortGetCoreID());

    char *buf = malloc(TASK_STATS_BUF_LEN);
    if (buf == NULL) {
        ESP_LOGE("task_stats", "malloc failed");
        vTaskDelete(NULL);
    }

    for (;;) {
        vTaskGetRunTimeStats(buf);
        ESP_LOGI("task_stats", "\nTask\t\tAbsTime\t\t%%CPU\n%s", buf);
        vTaskDelay(pdMS_TO_TICKS(TASK_STATS_INTERVAL_MS));
    }
}
#endif /* TASK_STATS_LOG_ENABLE && CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS */

void app_main(void)
{
#if CC1101_STANDALONE_DIAGNOSTIC
    cc1101_standalone_diagnostic();
    /* 진단 중에는 기존 FSM을 시작하지 않는다. 다른 장치 초기화 로그가 섞이면
     * CC1101 단독 결과를 판별할 수 없기 때문이다. */
    return;
#else
    fsm_init();

#if defined(TASK_STATS_LOG_ENABLE) && CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
    BaseType_t task_stats_ok = xTaskCreate(task_stats_task, "task_stats", 4096, NULL,
                                            TASK_STATS_PRIORITY, NULL);
    if (task_stats_ok != pdPASS) {
        ESP_LOGE("main", "task_stats_task create failed (err=%d)", (int)task_stats_ok);
    }
#endif

    /* TODO: 실제 주변장치 초기화(I2S/OLED/SPI/GPIO)가 끝난 뒤 아래 이벤트를 발생시킨다. */
    fsm_post_event(FSM_EVENT_INIT_DONE);
#endif
}
