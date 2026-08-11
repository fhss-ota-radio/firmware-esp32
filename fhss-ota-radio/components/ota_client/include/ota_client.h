#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* rf_transport가 상위 계층에 전달하는 CC1101 payload의 최대 크기. */
#define OTA_CLIENT_MAX_PACKET_LENGTH 60U

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

/*
 * RF 수신 버퍼의 packet_length 바이트를 OTA 전용 큐로 복사한다.
 * 호출이 반환된 뒤 호출자는 원본 버퍼를 즉시 재사용할 수 있다.
 */
esp_err_t ota_client_submit_packet(
    const uint8_t *packet,
    size_t packet_length
);

esp_err_t ota_client_abort(void);
ota_client_state_t ota_client_get_state(void);

#ifdef __cplusplus
}
#endif
