#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"

typedef struct {
    const esp_partition_t *partition;
    esp_ota_handle_t handle;

    size_t image_size;
    size_t written_size;

    bool active;
} ota_writer_t;

esp_err_t ota_writer_begin(
    ota_writer_t *writer,
    size_t image_size
);

esp_err_t ota_writer_write(
    ota_writer_t *writer,
    const void *data,
    size_t data_size
);

esp_err_t ota_writer_finish(
    ota_writer_t *writer,
    const uint8_t expected_sha256[32]
);

esp_err_t ota_writer_abort(
    ota_writer_t *writer
);
