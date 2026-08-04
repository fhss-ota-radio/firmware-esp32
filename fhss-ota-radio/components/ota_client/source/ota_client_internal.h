#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"

#include "esp_ota_ops.h"
#include "esp_partition.h"

#include "ota_client.h"

typedef struct {
    ota_client_state_t state;
    ota_client_config_t config;

    uint32_t session_id;
    uint32_t image_size;
    uint32_t received_bytes;

    uint32_t total_chunks;
    uint32_t expected_sequence;

    uint8_t expected_sha256[32];

    const esp_partition_t *update_partition;
    esp_ota_handle_t ota_handle;

    TickType_t last_packet_tick;
    bool ota_started;
} ota_client_context_t;