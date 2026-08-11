#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FHSS_SYNC_STATUS_OK = 0,
    FHSS_SYNC_STATUS_INVALID_ARG,
    FHSS_SYNC_STATUS_INVALID_CONFIG,
    FHSS_SYNC_STATUS_NOT_INITIALIZED,
} fhss_sync_status_t;

typedef enum {
    FHSS_SYNC_STATE_SEARCHING = 0,
    FHSS_SYNC_STATE_LOCKED,
} fhss_sync_state_t;

typedef enum {
    FHSS_SYNC_EVENT_NONE = 0,
    FHSS_SYNC_EVENT_ACQUIRED,
    FHSS_SYNC_EVENT_LOST,
} fhss_sync_event_t;

typedef struct {
    uint32_t acquire_count;
    uint32_t loss_count;
} fhss_sync_state_config_t;

typedef struct {
    fhss_sync_state_t state;
    uint32_t consecutive_valid;
    uint32_t consecutive_misses;
    fhss_sync_state_config_t config;
    uint8_t initialized;
} fhss_sync_state_tracker_t;

fhss_sync_status_t fhss_sync_state_init(fhss_sync_state_tracker_t *tracker,
                                         const fhss_sync_state_config_t *config);
fhss_sync_status_t fhss_sync_state_on_valid(fhss_sync_state_tracker_t *tracker,
                                             fhss_sync_event_t *out_event);
fhss_sync_status_t fhss_sync_state_on_miss(fhss_sync_state_tracker_t *tracker,
                                            fhss_sync_event_t *out_event);
fhss_sync_status_t fhss_sync_state_get(const fhss_sync_state_tracker_t *tracker,
                                        fhss_sync_state_t *out_state);

#ifdef __cplusplus
}
#endif
