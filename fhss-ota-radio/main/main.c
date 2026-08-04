#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_err.h"
#include "esp_image_format.h"
#include "esp_log.h"
#include "esp_flash.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"

static const char *TAG = "ota_practice";

#define OTA_COPY_CHUNK_SIZE 4096

static void configure_usb_console_input(void)
{
    usb_serial_jtag_vfs_set_rx_line_endings(ESP_LINE_ENDINGS_CR);
    usb_serial_jtag_vfs_set_tx_line_endings(ESP_LINE_ENDINGS_CRLF);

    usb_serial_jtag_driver_config_t config = {
        .tx_buffer_size = 256,
        .rx_buffer_size = 256,
    };

    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&config));
    usb_serial_jtag_vfs_use_driver();

    fcntl(fileno(stdout), F_SETFL, 0);
}

static void print_flash_size(void)
{
    uint32_t flash_size = 0;
    esp_err_t err = esp_flash_get_size(NULL, &flash_size);

    if (err == ESP_OK) {
        ESP_LOGI("FLASH", "Flash size: %lu bytes (%lu MB)",
                 (unsigned long)flash_size,
                 (unsigned long)(flash_size / (1024 * 1024)));
    } else {
        ESP_LOGE("FLASH", "Flash size read failed: %s",
                 esp_err_to_name(err));
    }
}

static const char *ota_state_name(esp_ota_img_states_t state)
{
    switch (state) {
    case ESP_OTA_IMG_NEW:
        return "NEW";
    case ESP_OTA_IMG_PENDING_VERIFY:
        return "PENDING_VERIFY";
    case ESP_OTA_IMG_VALID:
        return "VALID";
    case ESP_OTA_IMG_INVALID:
        return "INVALID";
    case ESP_OTA_IMG_ABORTED:
        return "ABORTED";
    case ESP_OTA_IMG_UNDEFINED:
        return "UNDEFINED";
    default:
        return "UNKNOWN";
    }
}

static void print_partition_info(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    esp_ota_img_states_t state;

    ESP_LOGI(TAG, "running: %-8s address=0x%06lx size=%lu KB",
             running->label, (unsigned long)running->address,
             (unsigned long)(running->size / 1024));
    ESP_LOGI(TAG, "boot:    %-8s address=0x%06lx", boot->label,
             (unsigned long)boot->address);
    ESP_LOGI(TAG, "next:    %-8s address=0x%06lx size=%lu KB",
             next->label, (unsigned long)next->address,
             (unsigned long)(next->size / 1024));

    esp_err_t err = esp_ota_get_state_partition(running, &state);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "OTA state: %s", ota_state_name(state));
    } else {
        ESP_LOGI(TAG, "OTA state: factory partition (no OTA state)");
    }
}

static esp_err_t clone_running_image_to_next_ota(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    esp_partition_pos_t source_position = {
        .offset = running->address,
        .size = running->size,
    };
    esp_image_metadata_t metadata = {0};
    esp_ota_handle_t ota_handle = 0;
    uint8_t *buffer = NULL;
    bool ota_open = false;
    esp_err_t err;

    ESP_LOGI(TAG, "Verifying image in '%s'...", running->label);
    err = esp_image_verify(ESP_IMAGE_VERIFY, &source_position, &metadata);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Source image verification failed: %s", esp_err_to_name(err));
        return err;
    }

    if (metadata.image_len > target->size) {
        ESP_LOGE(TAG, "Image (%lu bytes) is larger than target partition (%lu bytes)",
                 (unsigned long)metadata.image_len, (unsigned long)target->size);
        return ESP_ERR_INVALID_SIZE;
    }

    ESP_LOGI(TAG, "Copying %lu bytes: %s -> %s",
             (unsigned long)metadata.image_len, running->label, target->label);

    buffer = malloc(OTA_COPY_CHUNK_SIZE);
    if (buffer == NULL) {
        return ESP_ERR_NO_MEM;
    }

    err = esp_ota_begin(target, metadata.image_len, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        goto cleanup;
    }
    ota_open = true;

    for (size_t offset = 0; offset < metadata.image_len;) {
        size_t remaining = metadata.image_len - offset;
        size_t chunk_size = remaining < OTA_COPY_CHUNK_SIZE ? remaining : OTA_COPY_CHUNK_SIZE;

        err = esp_partition_read(running, offset, buffer, chunk_size);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Partition read failed at offset %lu: %s",
                     (unsigned long)offset, esp_err_to_name(err));
            goto cleanup;
        }

        err = esp_ota_write(ota_handle, buffer, chunk_size);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed at offset %lu: %s",
                     (unsigned long)offset, esp_err_to_name(err));
            goto cleanup;
        }
        offset += chunk_size;

        if ((offset % (64 * 1024)) == 0 || offset == metadata.image_len) {
            ESP_LOGI(TAG, "OTA copy progress: %lu / %lu bytes",
                     (unsigned long)offset, (unsigned long)metadata.image_len);
        }
    }

    err = esp_ota_end(ota_handle);
    ota_open = false;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end validation failed: %s", esp_err_to_name(err));
        goto cleanup;
    }

    err = esp_ota_set_boot_partition(target);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to select boot partition: %s", esp_err_to_name(err));
        goto cleanup;
    }

    ESP_LOGI(TAG, "OTA complete. Rebooting into '%s'...", target->label);
    free(buffer);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;

cleanup:
    if (ota_open) {
        esp_ota_abort(ota_handle);
    }
    free(buffer);
    return err;
}

static void read_usb_command(char *command, size_t capacity)
{
    size_t length = 0;

    while (true) {
        uint8_t character;
        int received = usb_serial_jtag_read_bytes(&character, 1, portMAX_DELAY);

        if (received <= 0) {
            continue;
        }

        if (character == '\r' || character == '\n') {
            if (length == 0) {
                continue;
            }
            command[length] = '\0';
            printf("\r\n");
            fflush(stdout);
            return;
        }

        if (character == '\b' || character == 0x7f) {
            if (length > 0) {
                length--;
                printf("\b \b");
                fflush(stdout);
            }
            continue;
        }

        if (character >= 0x20 && character <= 0x7e && length + 1 < capacity) {
            command[length++] = (char)character;
            putchar(character);
            fflush(stdout);
        }
    }
}

static void console_task(void *arg)
{
    char command[32];

    printf("\nOTA practice commands: info, ota, confirm, rollback, help\n");

    while (true) {
        printf("> ");
        fflush(stdout);
        read_usb_command(command, sizeof(command));

        if (strcmp(command, "info") == 0) {
            print_partition_info();
        } else if (strcmp(command, "ota") == 0) {
            clone_running_image_to_next_ota();
        } else if (strcmp(command, "confirm") == 0) {
            esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
            ESP_LOGI(TAG, "confirm: %s", esp_err_to_name(err));
        } else if (strcmp(command, "rollback") == 0) {
            esp_err_t err = esp_ota_mark_app_invalid_rollback_and_reboot();
            ESP_LOGE(TAG, "rollback failed: %s", esp_err_to_name(err));
        } else if (strcmp(command, "help") == 0) {
            printf("info     - show running/boot/next partitions\n");
            printf("ota      - copy the running image using esp_ota and reboot\n");
            printf("confirm  - mark a pending OTA image as valid\n");
            printf("rollback - mark it invalid and boot the previous image\n");
        } else if (command[0] != '\0') {
            printf("Unknown command: %s (type 'help')\n", command);
        }
    }
}

void app_main(void)
{
    configure_usb_console_input();
    print_flash_size();
    print_partition_info();
    xTaskCreate(console_task, "ota_console", 4096, NULL, 5, NULL);
}
