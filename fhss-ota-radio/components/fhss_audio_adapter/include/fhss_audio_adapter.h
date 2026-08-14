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

bool fhss_audio_adapter_init(const fhss_audio_adapter_config_t *config);
bool fhss_audio_adapter_begin_tx(void);
bool fhss_audio_adapter_submit_encoded_frame(
    const uint8_t *frame,
    size_t length
);
bool fhss_audio_adapter_end_tx(void);

#ifdef __cplusplus
}
#endif
