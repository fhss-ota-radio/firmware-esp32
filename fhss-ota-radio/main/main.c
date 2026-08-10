#include "esp_app_desc.h"
#include "esp_log.h"
#include "ota_test/ota_test_task.h"

static const char *TAG = "MAIN";

void app_main(void)
{
    ESP_LOGI(TAG, "Firmware version: %s", esp_app_get_description()->version);
    ESP_LOGI(TAG, "OTA test build ID: %s", OTA_TEST_BUILD_ID);
    ota_test_start();
}
