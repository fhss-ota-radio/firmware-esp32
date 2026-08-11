#include "fhss_slot_scheduler.h"

#include <limits.h>
#include <stddef.h>

static fhss_slot_status_t validate_scheduler(
    const fhss_slot_scheduler_t *scheduler,
    bool require_sync
)
{
    if (scheduler == NULL) {
        return FHSS_SLOT_STATUS_INVALID_ARG;
    }
    if (!scheduler->initialized) {
        return FHSS_SLOT_STATUS_NOT_INITIALIZED;
    }
    if (require_sync && !scheduler->synchronized) {
        return FHSS_SLOT_STATUS_NOT_SYNCHRONIZED;
    }
    return FHSS_SLOT_STATUS_OK;
}

fhss_slot_status_t fhss_slot_scheduler_init(
    fhss_slot_scheduler_t *scheduler,
    const fhss_slot_scheduler_config_t *config
)
{
    if (scheduler == NULL || config == NULL) {
        return FHSS_SLOT_STATUS_INVALID_ARG;
    }
    if (config->slot_duration_us == 0U ||
        config->channel_switch_guard_us >= config->slot_duration_us) {
        return FHSS_SLOT_STATUS_INVALID_CONFIG;
    }

    const fhss_slot_scheduler_t initialized_scheduler = {
        .config = *config,
        .reference_slot = 0U,
        .reference_time_us = 0,
        .initialized = true,
        .synchronized = false,
    };
    *scheduler = initialized_scheduler;
    return FHSS_SLOT_STATUS_OK;
}

fhss_slot_status_t fhss_slot_scheduler_set_reference(
    fhss_slot_scheduler_t *scheduler,
    uint32_t reference_slot,
    int64_t reference_time_us
)
{
    const fhss_slot_status_t status = validate_scheduler(scheduler, false);
    if (status != FHSS_SLOT_STATUS_OK) {
        return status;
    }
    if (reference_time_us < 0) {
        return FHSS_SLOT_STATUS_INVALID_ARG;
    }

    scheduler->reference_slot = reference_slot;
    scheduler->reference_time_us = reference_time_us;
    scheduler->synchronized = true;
    return FHSS_SLOT_STATUS_OK;
}

fhss_slot_status_t fhss_slot_scheduler_clear_reference(
    fhss_slot_scheduler_t *scheduler
)
{
    const fhss_slot_status_t status = validate_scheduler(scheduler, false);
    if (status != FHSS_SLOT_STATUS_OK) {
        return status;
    }

    scheduler->reference_slot = 0U;
    scheduler->reference_time_us = 0;
    scheduler->synchronized = false;
    return FHSS_SLOT_STATUS_OK;
}

fhss_slot_status_t fhss_slot_scheduler_get_slot(
    const fhss_slot_scheduler_t *scheduler,
    int64_t now_us,
    uint32_t *out_slot
)
{
    if (out_slot == NULL) {
        return FHSS_SLOT_STATUS_INVALID_ARG;
    }
    const fhss_slot_status_t status = validate_scheduler(scheduler, true);
    if (status != FHSS_SLOT_STATUS_OK) {
        return status;
    }
    if (now_us < scheduler->reference_time_us) {
        return FHSS_SLOT_STATUS_BEFORE_REFERENCE;
    }

    const uint64_t elapsed_us =
        (uint64_t)(now_us - scheduler->reference_time_us);
    const uint64_t elapsed_slots =
        elapsed_us / scheduler->config.slot_duration_us;
    if (elapsed_slots > (uint64_t)(UINT32_MAX - scheduler->reference_slot)) {
        return FHSS_SLOT_STATUS_OVERFLOW;
    }

    *out_slot = scheduler->reference_slot + (uint32_t)elapsed_slots;
    return FHSS_SLOT_STATUS_OK;
}

fhss_slot_status_t fhss_slot_scheduler_get_slot_start_time(
    const fhss_slot_scheduler_t *scheduler,
    uint32_t slot,
    int64_t *out_start_time_us
)
{
    if (out_start_time_us == NULL) {
        return FHSS_SLOT_STATUS_INVALID_ARG;
    }
    const fhss_slot_status_t status = validate_scheduler(scheduler, true);
    if (status != FHSS_SLOT_STATUS_OK) {
        return status;
    }
    if (slot < scheduler->reference_slot) {
        return FHSS_SLOT_STATUS_BEFORE_REFERENCE;
    }

    const uint64_t slot_delta = (uint64_t)(slot - scheduler->reference_slot);
    const uint64_t offset_us =
        slot_delta * (uint64_t)scheduler->config.slot_duration_us;
    if (offset_us > (uint64_t)(INT64_MAX - scheduler->reference_time_us)) {
        return FHSS_SLOT_STATUS_OVERFLOW;
    }

    *out_start_time_us = scheduler->reference_time_us + (int64_t)offset_us;
    return FHSS_SLOT_STATUS_OK;
}

fhss_slot_status_t fhss_slot_scheduler_get_next_switch_time(
    const fhss_slot_scheduler_t *scheduler,
    int64_t now_us,
    uint32_t *out_next_slot,
    int64_t *out_switch_time_us
)
{
    if (out_next_slot == NULL || out_switch_time_us == NULL) {
        return FHSS_SLOT_STATUS_INVALID_ARG;
    }

    uint32_t current_slot = 0U;
    fhss_slot_status_t status =
        fhss_slot_scheduler_get_slot(scheduler, now_us, &current_slot);
    if (status != FHSS_SLOT_STATUS_OK) {
        return status;
    }
    if (current_slot == UINT32_MAX) {
        return FHSS_SLOT_STATUS_OVERFLOW;
    }

    const uint32_t next_slot = current_slot + 1U;
    int64_t next_slot_start_us = 0;
    status = fhss_slot_scheduler_get_slot_start_time(
        scheduler,
        next_slot,
        &next_slot_start_us
    );
    if (status != FHSS_SLOT_STATUS_OK) {
        return status;
    }

    *out_next_slot = next_slot;
    *out_switch_time_us =
        next_slot_start_us - scheduler->config.channel_switch_guard_us;
    return FHSS_SLOT_STATUS_OK;
}
