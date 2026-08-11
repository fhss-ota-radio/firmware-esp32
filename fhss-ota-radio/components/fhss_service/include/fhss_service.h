#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fhss_fsm.h"
#include "fhss_diagnostics.h"
#include "fhss_sync_controller.h"
#include "rf_transport.h"

typedef enum {
    FHSS_SERVICE_ROLE_TX = 0,
    FHSS_SERVICE_ROLE_RX,
} fhss_service_role_t;

typedef enum {
    FHSS_SERVICE_EVENT_SYNC_ACQUIRED = 0,
    FHSS_SERVICE_EVENT_SYNC_LOST,
    FHSS_SERVICE_EVENT_ERROR,
} fhss_service_event_t;

typedef void (*fhss_service_event_callback_t)(
    fhss_service_event_t event,
    void *context
);

typedef struct {
    fhss_service_role_t role;
    rf_transport_config_t radio;
    const uint8_t *channels;
    size_t channel_count;
    uint32_t slot_duration_us;
    uint32_t channel_switch_guard_us;
    uint32_t sync_offset_us;
    uint32_t search_dwell_ms;
    uint32_t receive_timeout_ms;
    uint32_t acquire_count;
    uint32_t loss_count;
    uint32_t diagnostics_interval_ms;
    fhss_service_event_callback_t event_callback;
    void *event_context;
} fhss_service_config_t;

typedef struct {
    fhss_service_config_t config;
    rf_transport_t radio;
    fhss_sync_controller_t controller;
    fhss_fsm_t fsm;
    fhss_diagnostics_t diagnostics;
    void *diagnostics_mutex;
    void *task_handle;
    uint8_t current_channel;
    bool initialized;
} fhss_service_t;

bool fhss_service_init(
    fhss_service_t *service,
    const fhss_service_config_t *config
);

bool fhss_service_start(fhss_service_t *service);
fhss_fsm_state_t fhss_service_get_state(const fhss_service_t *service);
bool fhss_service_get_diagnostics(
    fhss_service_t *service,
    fhss_diagnostics_snapshot_t *out_snapshot
);
