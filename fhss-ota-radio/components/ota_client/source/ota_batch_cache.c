#include <string.h>

#include "ota_batch_cache.h"

static uint8_t ota_batch_required_mask(uint8_t chunk_count)
{
    return chunk_count == 0U
        ? 0U
        : (uint8_t)((1U << chunk_count) - 1U);
}

void ota_batch_cache_prepare(
    ota_batch_cache_t *cache,
    uint32_t base_sequence,
    uint32_t total_chunks
)
{
    if (cache == NULL) {
        return;
    }

    memset(cache, 0, sizeof(*cache));
    cache->base_sequence = base_sequence;

    if (base_sequence >= total_chunks) {
        return;
    }

    const uint32_t remaining = total_chunks - base_sequence;
    cache->chunk_count = (uint8_t)(
        remaining < OTA_CLIENT_BATCH_SIZE ? remaining : OTA_CLIENT_BATCH_SIZE
    );
}

esp_err_t ota_batch_cache_store(
    ota_batch_cache_t *cache,
    uint32_t sequence,
    const uint8_t *data,
    size_t data_size,
    bool *out_duplicate
)
{
    if (cache == NULL || data == NULL || data_size == 0U ||
        data_size > OTA_CLIENT_DATA_MAX_PAYLOAD_SIZE) {
        return ESP_ERR_INVALID_ARG;
    }
    if (cache->chunk_count == 0U || sequence < cache->base_sequence) {
        return ESP_ERR_INVALID_STATE;
    }

    const uint32_t index = sequence - cache->base_sequence;
    if (index >= cache->chunk_count) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint8_t bit = (uint8_t)(1U << index);
    const bool duplicate = (cache->received_mask & bit) != 0U;
    if (out_duplicate != NULL) {
        *out_duplicate = duplicate;
    }
    if (duplicate) {
        return ESP_OK;
    }

    memcpy(cache->data[index], data, data_size);
    cache->lengths[index] = (uint8_t)data_size;
    cache->received_mask |= bit;

    return ESP_OK;
}

uint8_t ota_batch_cache_missing_mask(const ota_batch_cache_t *cache)
{
    if (cache == NULL) {
        return 0U;
    }

    return (uint8_t)(
        ota_batch_required_mask(cache->chunk_count) &
        (uint8_t)~cache->received_mask
    );
}

bool ota_batch_cache_is_complete(const ota_batch_cache_t *cache)
{
    return cache != NULL && cache->chunk_count != 0U &&
           ota_batch_cache_missing_mask(cache) == 0U;
}

esp_err_t ota_batch_cache_commit(
    const ota_batch_cache_t *cache,
    ota_batch_write_callback_t write_callback,
    void *context,
    size_t *out_written_size
)
{
    if (cache == NULL || write_callback == NULL || out_written_size == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!ota_batch_cache_is_complete(cache)) {
        return ESP_ERR_INVALID_STATE;
    }

    size_t written_size = 0U;
    for (uint8_t i = 0U; i < cache->chunk_count; ++i) {
        const esp_err_t err = write_callback(
            cache->data[i],
            cache->lengths[i],
            context
        );
        if (err != ESP_OK) {
            return err;
        }
        written_size += cache->lengths[i];
    }

    *out_written_size = written_size;
    return ESP_OK;
}
