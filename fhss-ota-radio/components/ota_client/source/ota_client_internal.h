#pragma once

#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "ota_client.h"
#include "ota_batch_cache.h"
#include "ota_writer.h"

#define OTA_CLIENT_RX_QUEUE_DEPTH 16U

typedef struct {
    uint8_t data[OTA_CLIENT_MAX_PACKET_LENGTH];
    uint8_t length;
} ota_client_rx_packet_t;

typedef struct {
    ota_client_state_t state;
    ota_client_config_t config;
    QueueHandle_t rx_queue;

    ota_writer_t writer;

    uint32_t session_id;
    uint32_t image_size;
    uint32_t received_bytes;

    uint32_t total_chunks;
    uint32_t expected_sequence;
    ota_batch_cache_t batch_cache;
    uint32_t last_committed_batch_base;
    uint8_t last_committed_batch_count;
    bool has_committed_batch;
    uint8_t expected_sha256[32];
    TickType_t last_packet_tick;
} ota_client_context_t;

/* ota-protocol 디코더를 연결할 OTA 태스크가 큐를 소비할 때 사용하는 내부 API. */
esp_err_t ota_client_receive_packet(
    ota_client_rx_packet_t *out_packet,
    TickType_t wait_ticks
);

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

typedef struct {
    uint32_t base_sequence;
    uint32_t next_sequence;
    uint8_t chunk_count;
    uint8_t missing_mask;
    bool committed;
} ota_client_batch_result_t;

/* BATCH_CHECK 수신 시 누락 bitmap을 계산하고, 완성된 배치를 순차 기록한다. */
esp_err_t ota_client_check_batch(
    uint32_t session_id,
    uint32_t batch_base_sequence,
    ota_client_batch_result_t *out_result
);

esp_err_t ota_client_finish_session(
    uint32_t session_id
);
