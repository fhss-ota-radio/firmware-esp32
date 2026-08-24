#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "ota_protocol.h"

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
    OTA_CLIENT_EVENT_FHSS_CONFIG_READY,
    OTA_CLIENT_EVENT_FHSS_ACTIVATING,
    OTA_CLIENT_EVENT_FHSS_ACTIVATE_FAILED,
    OTA_CLIENT_EVENT_STARTED,
    OTA_CLIENT_EVENT_PROGRESS,
    OTA_CLIENT_EVENT_APPLYING,
    OTA_CLIENT_EVENT_COMPLETED,
    OTA_CLIENT_EVENT_FAILED,
    OTA_CLIENT_EVENT_ABORTED,
} ota_client_event_t;

/* true일 때만 DISCOVER에 응답하고 새 START 세션을 허용한다. 제품에서는
 * FSM_STATE_MENU_OTA 여부를 반환하는 callback을 연결한다. */
typedef bool (*ota_client_ota_mode_callback_t)(void *context);

/* DISCOVER 응답 충돌 회피에 사용할 난수 원본. 반환값은
 * discover_backoff_max_ms 범위로 축소되며, 제품에서는 esp_random()을 연결한다. */
typedef uint32_t (*ota_client_random_callback_t)(void *context);

/* Called only after a matching FHSS_ACTIVATE ACK has been transmitted. The
 * radio owner uses this hook to leave bootstrap channel 0 and start the
 * pending generation as an FHSS slave. */
typedef esp_err_t (*ota_client_fhss_activate_callback_t)(
    const ota_fhss_config_fields_t *config,
    void *context
);

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
    uint8_t firmware_version[3];
    uint32_t receive_timeout_ms;
    uint32_t discover_backoff_max_ms;

    ota_client_send_callback_t send_callback;
    ota_client_event_callback_t event_callback;
    ota_client_ota_mode_callback_t ota_mode_callback;
    ota_client_random_callback_t random_callback;
    ota_client_fhss_activate_callback_t fhss_activate_callback;

    void *callback_context;
} ota_client_config_t;

esp_err_t ota_client_init(const ota_client_config_t *config);

/* init 이후 OTA RX Queue를 계속 소비하는 전용 Task를 한 번 생성한다. */
esp_err_t ota_client_start_consumer(void);

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
