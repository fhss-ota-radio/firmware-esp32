#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef bool (*fhss_audio_adapter_rx_frame_callback_t)(
    const uint8_t *frame,
    size_t length,
    void *context
);

typedef enum {
    FHSS_AUDIO_ADAPTER_EVENT_SYNC_LOST = 0,
    FHSS_AUDIO_ADAPTER_EVENT_TALKSPURT_ENDED,
    FHSS_AUDIO_ADAPTER_EVENT_ERROR,
} fhss_audio_adapter_event_t;

typedef void (*fhss_audio_adapter_event_callback_t)(
    fhss_audio_adapter_event_t event,
    void *context
);

typedef struct {
    fhss_audio_adapter_rx_frame_callback_t rx_frame_callback;
    fhss_audio_adapter_event_callback_t event_callback;
    void *callback_context;
} fhss_audio_adapter_config_t;

typedef enum {
    FHSS_AUDIO_ADAPTER_OTA_RX_OK = 0,
    FHSS_AUDIO_ADAPTER_OTA_RX_TIMEOUT,
    FHSS_AUDIO_ADAPTER_OTA_RX_CRC_ERROR,
    FHSS_AUDIO_ADAPTER_OTA_RX_ERROR,
} fhss_audio_adapter_ota_rx_status_t;

bool fhss_audio_adapter_init(const fhss_audio_adapter_config_t *config);
bool fhss_audio_adapter_begin_tx(void);
bool fhss_audio_adapter_submit_encoded_frame(
    const uint8_t *frame,
    size_t length
);
bool fhss_audio_adapter_end_tx(void);
bool fhss_audio_adapter_begin_ota(void);
bool fhss_audio_adapter_end_ota(void);
fhss_audio_adapter_ota_rx_status_t fhss_audio_adapter_ota_receive(
    uint8_t *packet,
    size_t capacity,
    size_t *out_length,
    uint32_t timeout_ms
);
bool fhss_audio_adapter_ota_send(const uint8_t *packet, size_t length);

#ifdef __cplusplus
}
#endif
