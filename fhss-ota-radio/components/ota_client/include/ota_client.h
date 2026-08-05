#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    OTA_CLIENT_STATE_UNINITIALIZED = 0,
    OTA_CLIENT_STATE_IDLE,
    OTA_CLIENT_STATE_RECEIVING,
    OTA_CLIENT_STATE_VERIFYING,
    OTA_CLIENT_STATE_READY_TO_REBOOT,
    OTA_CLIENT_STATE_ERROR,
} ota_client_state_t;

typedef enum {
    OTA_CLIENT_EVENT_STARTED,
    OTA_CLIENT_EVENT_PROGRESS,
    OTA_CLIENT_EVENT_COMPLETED,
    OTA_CLIENT_EVENT_FAILED,
    OTA_CLIENT_EVENT_ABORTED,
} ota_client_event_t;

typedef esp_err_t (*ota_client_send_callback_t)(
    const uint8_t *packet,
    size_t packet_length,
    void *context
);

typedef void (*ota_client_event_callback_t)(
    ota_client_event_t event,
    uint32_t progress_percent,
    esp_err_t error,
    void *context
);

typedef struct {
    uint32_t device_id;
    uint32_t receive_timeout_ms;

    ota_client_send_callback_t send_callback;
    ota_client_event_callback_t event_callback;

    void *callback_context;
} ota_client_config_t;

esp_err_t ota_client_init(const ota_client_config_t *config);

ota_client_state_t ota_client_get_state(void);

#ifdef __cplusplus
}
#endif