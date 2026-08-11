#include "fhss_sync_state.h"

#include <stddef.h>

static fhss_sync_status_t validate_tracker(const fhss_sync_state_tracker_t *tracker)
{
    if (tracker == NULL) {
        return FHSS_SYNC_STATUS_INVALID_ARG;
    }
    if (tracker->initialized == 0U) {
        return FHSS_SYNC_STATUS_NOT_INITIALIZED;
    }
    return FHSS_SYNC_STATUS_OK;
}

fhss_sync_status_t fhss_sync_state_init(fhss_sync_state_tracker_t *tracker,
                                         const fhss_sync_state_config_t *config)
{
    if (tracker == NULL || config == NULL) {
        return FHSS_SYNC_STATUS_INVALID_ARG;
    }
    if (config->acquire_count == 0U || config->loss_count == 0U) {
        return FHSS_SYNC_STATUS_INVALID_CONFIG;
    }

    const fhss_sync_state_tracker_t initialized_tracker = {
        .state = FHSS_SYNC_STATE_SEARCHING,
        .consecutive_valid = 0U,
        .consecutive_misses = 0U,
        .config = *config,
        .initialized = 1U,
    };
    *tracker = initialized_tracker;
    return FHSS_SYNC_STATUS_OK;
}

fhss_sync_status_t fhss_sync_state_on_valid(fhss_sync_state_tracker_t *tracker,
                                             fhss_sync_event_t *out_event)
{
    if (out_event == NULL) {
        return FHSS_SYNC_STATUS_INVALID_ARG;
    }
    const fhss_sync_status_t status = validate_tracker(tracker);
    if (status != FHSS_SYNC_STATUS_OK) {
        return status;
    }

    *out_event = FHSS_SYNC_EVENT_NONE;
    tracker->consecutive_misses = 0U;
    if (tracker->state == FHSS_SYNC_STATE_SEARCHING) {
        tracker->consecutive_valid++;
        if (tracker->consecutive_valid >= tracker->config.acquire_count) {
            tracker->state = FHSS_SYNC_STATE_LOCKED;
            tracker->consecutive_valid = 0U;
            *out_event = FHSS_SYNC_EVENT_ACQUIRED;
        }
    }
    return FHSS_SYNC_STATUS_OK;
}

fhss_sync_status_t fhss_sync_state_on_miss(fhss_sync_state_tracker_t *tracker,
                                            fhss_sync_event_t *out_event)
{
    if (out_event == NULL) {
        return FHSS_SYNC_STATUS_INVALID_ARG;
    }
    const fhss_sync_status_t status = validate_tracker(tracker);
    if (status != FHSS_SYNC_STATUS_OK) {
        return status;
    }

    *out_event = FHSS_SYNC_EVENT_NONE;
    tracker->consecutive_valid = 0U;
    if (tracker->state == FHSS_SYNC_STATE_LOCKED) {
        tracker->consecutive_misses++;
        if (tracker->consecutive_misses >= tracker->config.loss_count) {
            tracker->state = FHSS_SYNC_STATE_SEARCHING;
            tracker->consecutive_misses = 0U;
            *out_event = FHSS_SYNC_EVENT_LOST;
        }
    }
    return FHSS_SYNC_STATUS_OK;
}

fhss_sync_status_t fhss_sync_state_get(const fhss_sync_state_tracker_t *tracker,
                                        fhss_sync_state_t *out_state)
{
    if (out_state == NULL) {
        return FHSS_SYNC_STATUS_INVALID_ARG;
    }
    const fhss_sync_status_t status = validate_tracker(tracker);
    if (status != FHSS_SYNC_STATUS_OK) {
        return status;
    }
    *out_state = tracker->state;
    return FHSS_SYNC_STATUS_OK;
}
