#include <stdio.h>
#include <stdlib.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "fsm.h"

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
}
