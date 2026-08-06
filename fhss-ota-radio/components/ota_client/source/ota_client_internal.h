#pragma once

#include <stdint.h>
#include "freertos/FreeRTOS.h"

#include "ota_client.h"
#include "ota_writer.h"

typedef struct {
    ota_client_state_t state;
    ota_client_config_t config;

    ota_writer_t writer;

    uint32_t session_id;
    uint32_t image_size;
    uint32_t received_bytes;

    uint32_t total_chunks;
    uint32_t expected_sequence;
    uint8_t expected_sha256[32];
    TickType_t last_packet_tick;
} ota_client_context_t;

esp_err_t ota_client_start_session(
    uint32_t session_id,
    uint32_t image_size,
    uint32_t total_chunks
);

esp_err_t ota_client_write_chunk(
    uint32_t session_id,
    uint32_t sequence,
    const uint8_t *data,
    size_t data_size
);