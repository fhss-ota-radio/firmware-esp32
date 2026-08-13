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

    size_t batch_size = 0U;
    for (uint8_t i = 0U; i < cache->chunk_count; ++i) {
        /* data[5][48]을 하나의 연속 buffer로 전달하려면 마지막 slot을 제외한
         * 모든 slot이 정확히 48B여야 한다. 제품 경로에서는 write_chunk가
         * 이미지 크기를 기준으로 이 조건을 먼저 검증한다. */
        if (cache->lengths[i] == 0U ||
            (i + 1U < cache->chunk_count &&
             cache->lengths[i] != OTA_CLIENT_DATA_MAX_PAYLOAD_SIZE)) {
            return ESP_ERR_INVALID_SIZE;
        }
        batch_size += cache->lengths[i];
    }

    /* 5×48B 배열은 메모리상 연속이다. 완성된 배치를 한 번의 writer 호출로
     * 넘겨 esp_ota_write()도 배치당 정확히 한 번만 실행되게 한다. */
    const esp_err_t err = write_callback(
        &cache->data[0][0],
        batch_size,
        context
    );
    if (err != ESP_OK) {
        return err;
    }

    *out_written_size = batch_size;
    return ESP_OK;
}
