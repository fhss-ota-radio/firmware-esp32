#pragma once

#include <stddef.h>
#include <stdint.h>

#include "fhss_core.h"
#include "fhss_slot_scheduler.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FHSS_CONTROLLER_STATUS_OK = 0,
    FHSS_CONTROLLER_STATUS_INVALID_ARG,
    FHSS_CONTROLLER_STATUS_NOT_INITIALIZED,
    FHSS_CONTROLLER_STATUS_PACKET_ERROR,
    FHSS_CONTROLLER_STATUS_CORE_ERROR,
    FHSS_CONTROLLER_STATUS_SCHEDULER_ERROR,
} fhss_sync_controller_status_t;

typedef struct {
    fhss_core_config_t core;
    fhss_slot_scheduler_config_t scheduler;
    uint32_t sync_offset_us;
    uint32_t correction_deadband_us;
    uint32_t correction_fast_threshold_us;
    uint32_t correction_slow_divisor;
    uint32_t correction_fast_divisor;
} fhss_sync_controller_config_t;

typedef struct {
    fhss_core_t core;
    fhss_slot_scheduler_t scheduler;
    uint32_t sync_offset_us;
    uint32_t correction_deadband_us;
    uint32_t correction_fast_threshold_us;
    uint32_t correction_slow_divisor;
    uint32_t correction_fast_divisor;
    int64_t last_phase_correction_us;
    int64_t accumulated_phase_correction_us;
    bool initialized;
} fhss_sync_controller_t;

fhss_sync_controller_status_t fhss_sync_controller_init(
    fhss_sync_controller_t *controller,
    const fhss_sync_controller_config_t *config
);

fhss_sync_controller_status_t fhss_sync_controller_process_rx(
    fhss_sync_controller_t *controller,
    const uint8_t *buffer,
    size_t buffer_length,
    int64_t rx_timestamp_us,
    fhss_core_rx_result_t *out_result
);

/* Validate a SYNC packet and deliberately re-anchor the scheduler to its
 * observed timestamp. This is only for bounded recovery after normal timing
 * validation has degraded; initial acquisition still uses process_rx(). */
fhss_sync_controller_status_t fhss_sync_controller_recover_rx(
    fhss_sync_controller_t *controller,
    const uint8_t *buffer,
    size_t buffer_length,
    int64_t rx_timestamp_us,
    fhss_core_rx_result_t *out_result
);

fhss_sync_controller_status_t fhss_sync_controller_handle_timeout(
    fhss_sync_controller_t *controller,
    fhss_sync_event_t *out_event,
    fhss_sync_state_t *out_state
);

#ifdef __cplusplus
}
#endif
