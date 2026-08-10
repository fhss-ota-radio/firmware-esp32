#include "ota_test_task.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_app_desc.h"
#include "esp_image_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "ota_client.h"

static const char *TAG = "OTA_TEST";

#define OTA_TEST_CHUNK_SIZE 4096U
#define OTA_TEST_SESSION_ID 0x4C4F4341U

static esp_err_t local_send_callback(
    const uint8_t *packet,
    size_t packet_length,
    void *context
)
{
    (void)packet;
    (void)packet_length;
    (void)context;
    return ESP_OK;
}

static void print_partition_info(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *update = esp_ota_get_next_update_partition(NULL);

    if (running != NULL) {
        ESP_LOGI(TAG, "Running partition: %s, address=0x%08" PRIx32 ", size=%" PRIu32,
                 running->label, running->address, running->size);
    } else {
        ESP_LOGE(TAG, "Failed to get running partition");
    }

    if (update != NULL) {
        ESP_LOGI(TAG, "Next update partition: %s, address=0x%08" PRIx32 ", size=%" PRIu32,
                 update->label, update->address, update->size);
    } else {
        ESP_LOGE(TAG, "Failed to get update partition");
    }
}

static esp_err_t get_valid_image_size(
    const esp_partition_t *partition,
    uint32_t *image_size
)
{
    if (partition == NULL || image_size == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const esp_partition_pos_t position = {
        .offset = partition->address,
        .size = partition->size,
    };
    esp_image_metadata_t metadata = {0};
    esp_err_t err = esp_image_verify(
        ESP_IMAGE_VERIFY_SILENT,
        &position,
        &metadata
    );
    if (err != ESP_OK) {
        return err;
    }
    if (metadata.image_len == 0 || metadata.image_len > partition->size) {
        return ESP_ERR_INVALID_SIZE;
    }

    *image_size = metadata.image_len;
    return ESP_OK;
}

static esp_err_t stream_partition_to_next_ota(
    const esp_partition_t *source,
    esp_partition_subtype_t expected_target_subtype
)
{
    uint32_t image_size = 0;
    esp_err_t err = get_valid_image_size(source, &image_size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "No valid image in %s: %s", source->label, esp_err_to_name(err));
        return err;
    }

    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (target == NULL) {
        ESP_LOGE(TAG, "No OTA update partition found");
        return ESP_ERR_NOT_FOUND;
    }
    if (target->subtype != expected_target_subtype) {
        ESP_LOGE(TAG, "Unexpected target %s (subtype 0x%02x)",
                 target->label, target->subtype);
        return ESP_ERR_INVALID_STATE;
    }

    const uint32_t total_chunks =
        (image_size + OTA_TEST_CHUNK_SIZE - 1U) / OTA_TEST_CHUNK_SIZE;
    const ota_client_config_t config = {
        .device_id = 0,
        .receive_timeout_ms = 30000,
        .send_callback = local_send_callback,
        .event_callback = NULL,
        .callback_context = NULL,
    };

    ESP_LOGI(TAG,
             "Writing %s (%" PRIu32 " bytes) to %s in %" PRIu32 " chunks",
             source->label, image_size, target->label, total_chunks);

    err = ota_client_init(&config);
    if (err != ESP_OK) {
        return err;
    }
    err = ota_client_start_session(OTA_TEST_SESSION_ID, image_size, total_chunks);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t *buffer = malloc(OTA_TEST_CHUNK_SIZE);
    if (buffer == NULL) {
        ota_client_abort();
        return ESP_ERR_NO_MEM;
    }

    uint32_t offset = 0;
    uint32_t last_reported_percent = 0;
    for (uint32_t sequence = 0; sequence < total_chunks; sequence++) {
        size_t chunk_size = image_size - offset;
        if (chunk_size > OTA_TEST_CHUNK_SIZE) {
            chunk_size = OTA_TEST_CHUNK_SIZE;
        }

        err = esp_partition_read(source, offset, buffer, chunk_size);
        if (err == ESP_OK) {
            err = ota_client_write_chunk(
                OTA_TEST_SESSION_ID,
                sequence,
                buffer,
                chunk_size
            );
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "OTA write failed at offset 0x%" PRIx32 ": %s",
                     offset, esp_err_to_name(err));
            free(buffer);
            ota_client_abort();
            return err;
        }

        offset += chunk_size;
        uint32_t percent = (uint32_t)(((uint64_t)offset * 100U) / image_size);
        if (percent == 100U || percent >= last_reported_percent + 10U) {
            ESP_LOGI(TAG, "OTA progress: %" PRIu32 "%%", percent);
            last_reported_percent = percent;
        }
    }
    free(buffer);

    err = ota_client_finish_session(OTA_TEST_SESSION_ID);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA finish failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "OTA image verified; next boot partition is %s", target->label);
    return ESP_OK;
}

static void ota_test_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "OTA test task started");
    print_partition_info();

    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running == NULL) {
        ESP_LOGE(TAG, "TEST FAIL: running partition not found");
        vTaskDelete(NULL);
        return;
    }

    esp_err_t err;
    if (running->subtype == ESP_PARTITION_SUBTYPE_APP_FACTORY) {
        ESP_LOGI(TAG, "Stage 1/3: bootstrap factory image into ota_0");
        err = stream_partition_to_next_ota(
            running,
            ESP_PARTITION_SUBTYPE_APP_OTA_0
        );
    } else if (running->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0) {
        ESP_LOGI(TAG, "Stage 2/3: write staged candidate from storage into ota_1");
        const esp_partition_t *candidate = esp_partition_find_first(
            ESP_PARTITION_TYPE_DATA,
            ESP_PARTITION_SUBTYPE_ANY,
            "storage"
        );
        if (candidate == NULL) {
            ESP_LOGE(TAG, "TEST FAIL: storage partition not found");
            vTaskDelete(NULL);
            return;
        }
        err = stream_partition_to_next_ota(
            candidate,
            ESP_PARTITION_SUBTYPE_APP_OTA_1
        );
    } else if (running->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_1) {
        const esp_app_desc_t *app = esp_app_get_description();
        ESP_LOGI(TAG, "Stage 3/3: candidate booted from ota_1");
        if (strcmp(OTA_TEST_BUILD_ID, "candidate") != 0) {
            ESP_LOGE(TAG,
                     "TEST FAIL: ota_1 contains build_id=%s, expected candidate",
                     OTA_TEST_BUILD_ID);
            vTaskDelete(NULL);
            return;
        }
        ESP_LOGI(TAG, "TEST PASS: A/B OTA completed, version=%s, build_id=%s",
                 app->version, OTA_TEST_BUILD_ID);
        vTaskDelete(NULL);
        return;
    } else {
        ESP_LOGE(TAG, "TEST FAIL: unsupported running partition %s", running->label);
        vTaskDelete(NULL);
        return;
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "TEST FAIL: %s", esp_err_to_name(err));
        ESP_LOGI(TAG, "Fix/stage the image, then reset the board to retry");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Rebooting to continue A/B OTA test");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();

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
