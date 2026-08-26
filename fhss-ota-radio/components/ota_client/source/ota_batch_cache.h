#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define OTA_CLIENT_BATCH_SIZE 5U
/* ota-protocol v0.2 (9d2aa2b): RF body 60B - DATA header 12B. */
#define OTA_CLIENT_DATA_MAX_PAYLOAD_SIZE 48U

typedef struct {
    uint32_t base_sequence;
    uint8_t chunk_count;
    uint8_t received_mask;
    uint8_t lengths[OTA_CLIENT_BATCH_SIZE];
    uint8_t data[OTA_CLIENT_BATCH_SIZE][OTA_CLIENT_DATA_MAX_PAYLOAD_SIZE];
} ota_batch_cache_t;

typedef esp_err_t (*ota_batch_write_callback_t)(
    const uint8_t *data,
    size_t data_size,
    void *context
);

void ota_batch_cache_prepare(
    ota_batch_cache_t *cache,
    uint32_t base_sequence,
    uint32_t total_chunks
);

esp_err_t ota_batch_cache_store(
    ota_batch_cache_t *cache,
    uint32_t sequence,
    const uint8_t *data,
    size_t data_size,
    bool *out_duplicate
);

uint8_t ota_batch_cache_missing_mask(const ota_batch_cache_t *cache);
bool ota_batch_cache_is_complete(const ota_batch_cache_t *cache);

esp_err_t ota_batch_cache_commit(
    const ota_batch_cache_t *cache,
    ota_batch_write_callback_t write_callback,
    void *context,
    size_t *out_written_size
);
