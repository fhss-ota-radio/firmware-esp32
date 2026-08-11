#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FHSS_SLOT_STATUS_OK = 0,
    FHSS_SLOT_STATUS_INVALID_ARG,
    FHSS_SLOT_STATUS_INVALID_CONFIG,
    FHSS_SLOT_STATUS_NOT_INITIALIZED,
    FHSS_SLOT_STATUS_NOT_SYNCHRONIZED,
    FHSS_SLOT_STATUS_BEFORE_REFERENCE,
    FHSS_SLOT_STATUS_OVERFLOW,
} fhss_slot_status_t;

typedef struct {
    uint32_t slot_duration_us;
    uint32_t channel_switch_guard_us;
} fhss_slot_scheduler_config_t;

typedef struct {
    fhss_slot_scheduler_config_t config;
    uint32_t reference_slot;
    int64_t reference_time_us;
    bool initialized;
    bool synchronized;
} fhss_slot_scheduler_t;

fhss_slot_status_t fhss_slot_scheduler_init(
    fhss_slot_scheduler_t *scheduler,
    const fhss_slot_scheduler_config_t *config
);

fhss_slot_status_t fhss_slot_scheduler_set_reference(
    fhss_slot_scheduler_t *scheduler,
    uint32_t reference_slot,
    int64_t reference_time_us
);

fhss_slot_status_t fhss_slot_scheduler_clear_reference(
    fhss_slot_scheduler_t *scheduler
);

fhss_slot_status_t fhss_slot_scheduler_get_slot(
    const fhss_slot_scheduler_t *scheduler,
    int64_t now_us,
    uint32_t *out_slot
);

fhss_slot_status_t fhss_slot_scheduler_get_slot_start_time(
    const fhss_slot_scheduler_t *scheduler,
    uint32_t slot,
    int64_t *out_start_time_us
);

fhss_slot_status_t fhss_slot_scheduler_get_next_switch_time(
    const fhss_slot_scheduler_t *scheduler,
    int64_t now_us,
    uint32_t *out_next_slot,
    int64_t *out_switch_time_us
);

#ifdef __cplusplus
}
#endif
