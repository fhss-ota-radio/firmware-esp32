#include "ota_writer.h"

esp_err_t ota_writer_begin(
    ota_writer_t *writer,
    size_t image_size
)
{
    if (writer == NULL || image_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (writer->active) {
        return ESP_ERR_INVALID_STATE;
    }

    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (update_partition == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    if (image_size > update_partition->size) {
        return ESP_ERR_INVALID_SIZE;
    }
    
    esp_ota_handle_t handle = 0; // 값 변경될 위험 있으므로 새 변수에 
    esp_err_t err = esp_ota_begin(update_partition, image_size, &handle);
    
    if (err != ESP_OK) {
        return err;
    }

    writer->partition = update_partition;
    writer->handle = handle;
    writer->image_size = image_size;
    writer->written_size = 0;
    writer->active = true;

    return ESP_OK;
}

esp_err_t ota_writer_write(
    ota_writer_t *writer,
    const void *data,
    size_t data_size
)
{
    if (writer == NULL || data == NULL || data_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!writer->active) {
        return ESP_ERR_INVALID_STATE;
    }

    if (writer->written_size > writer->image_size ||
        data_size > writer->image_size - writer->written_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t err = esp_ota_write(
        writer->handle,
        data,
        data_size
    );

    if (err != ESP_OK) {
        return err;
    }

    writer->written_size += data_size;

    return ESP_OK;
}

esp_err_t ota_writer_finish(
    ota_writer_t *writer
)
{
    if (writer == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!writer->active || writer->partition == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (writer->written_size != writer->image_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t err = esp_ota_end(writer->handle);

    /*
     * esp_ota_end() 호출 후에는 성공 여부와 관계없이
     * handle이 더 이상 유효하지 않다.
     */
    writer->handle = 0;
    writer->active = false;

    if (err != ESP_OK) {
        return err;
    }

    err = esp_ota_set_boot_partition(writer->partition);

    if (err != ESP_OK) {
        return err;
    }

    return ESP_OK;
}

