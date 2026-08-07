#include "ota_test_task.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "OTA_TEST";

static void ota_test_task(void *arg)
{
    ESP_LOGI(TAG, "OTA test task started");

    // OTA Test Code
    vTaskDelete(NULL);
}

void ota_test_start(void)
{
    xTaskCreate(
        ota_test_task,
        "ota_test_task",
        8192,
        NULL,
        5,
        NULL
    );
}