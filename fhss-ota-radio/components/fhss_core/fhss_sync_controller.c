#include "fhss_sync_controller.h"

#include <stdbool.h>

fhss_sync_controller_status_t fhss_sync_controller_init(
    fhss_sync_controller_t *controller,
    const fhss_sync_controller_config_t *config
)
{
    if (controller == NULL || config == NULL) {
        return FHSS_CONTROLLER_STATUS_INVALID_ARG;
    }

    fhss_sync_controller_t initialized = {
        .sync_offset_us = config->sync_offset_us,
        .initialized = false,
    };

    if (fhss_core_init(&initialized.core, &config->core) !=
        FHSS_CORE_STATUS_OK) {
        return FHSS_CONTROLLER_STATUS_CORE_ERROR;
    }
    if (fhss_slot_scheduler_init(&initialized.scheduler, &config->scheduler) !=
        FHSS_SLOT_STATUS_OK) {
        return FHSS_CONTROLLER_STATUS_SCHEDULER_ERROR;
    }

    initialized.initialized = true;
    *controller = initialized;
    return FHSS_CONTROLLER_STATUS_OK;
}

fhss_sync_controller_status_t fhss_sync_controller_process_rx(
    fhss_sync_controller_t *controller,
    const uint8_t *buffer,
    size_t buffer_length,
    int64_t rx_timestamp_us,
    fhss_core_rx_result_t *out_result
)
{
    if (controller == NULL || buffer == NULL || out_result == NULL ||
        rx_timestamp_us < 0) {
        return FHSS_CONTROLLER_STATUS_INVALID_ARG;
    }
    if (!controller->initialized) {
        return FHSS_CONTROLLER_STATUS_NOT_INITIALIZED;
    }

    fhss_sync_packet_t packet = {0};
    if (fhss_sync_packet_decode(buffer, buffer_length, &packet) !=
        FHSS_PACKET_STATUS_OK) {
        return FHSS_CONTROLLER_STATUS_PACKET_ERROR;
    }

    if (rx_timestamp_us < (int64_t)controller->sync_offset_us) {
        return FHSS_CONTROLLER_STATUS_INVALID_ARG;
    }
    const int64_t observed_slot_start_us =
        rx_timestamp_us - (int64_t)controller->sync_offset_us;

    int64_t expected_rx_time_us = rx_timestamp_us;
    if (controller->scheduler.synchronized) {
        int64_t expected_slot_start_us = 0;
        if (fhss_slot_scheduler_get_slot_start_time(
                &controller->scheduler,
                packet.slot_number,
                &expected_slot_start_us) != FHSS_SLOT_STATUS_OK) {
            return FHSS_CONTROLLER_STATUS_SCHEDULER_ERROR;
        }
        expected_rx_time_us =
            expected_slot_start_us + (int64_t)controller->sync_offset_us;
    }

    fhss_core_rx_result_t result = {0};
    if (fhss_core_process_rx(
            &controller->core,
            buffer,
            buffer_length,
            expected_rx_time_us,
            rx_timestamp_us,
            &result) != FHSS_CORE_STATUS_OK) {
        return FHSS_CONTROLLER_STATUS_CORE_ERROR;
    }

    if (!controller->scheduler.synchronized ||
        result.timing.result == FHSS_TIMING_INSIDE_WINDOW) {
        if (fhss_slot_scheduler_set_reference(
                &controller->scheduler,
                packet.slot_number,
                observed_slot_start_us) != FHSS_SLOT_STATUS_OK) {
            return FHSS_CONTROLLER_STATUS_SCHEDULER_ERROR;
        }
    }

    *out_result = result;
    return FHSS_CONTROLLER_STATUS_OK;
}

fhss_sync_controller_status_t fhss_sync_controller_handle_timeout(
    fhss_sync_controller_t *controller,
    fhss_sync_event_t *out_event,
    fhss_sync_state_t *out_state
)
{
    if (controller == NULL || out_event == NULL || out_state == NULL) {
        return FHSS_CONTROLLER_STATUS_INVALID_ARG;
    }
    if (!controller->initialized) {
        return FHSS_CONTROLLER_STATUS_NOT_INITIALIZED;
    }

    if (fhss_core_handle_timeout(
            &controller->core,
            out_event,
            out_state) != FHSS_CORE_STATUS_OK) {
        return FHSS_CONTROLLER_STATUS_CORE_ERROR;
    }

    if (*out_event == FHSS_SYNC_EVENT_LOST) {
        if (fhss_slot_scheduler_clear_reference(&controller->scheduler) !=
            FHSS_SLOT_STATUS_OK) {
            return FHSS_CONTROLLER_STATUS_SCHEDULER_ERROR;
        }
    }
    return FHSS_CONTROLLER_STATUS_OK;
}

fhss_sync_controller_status_t fhss_sync_controller_recover_rx(
    fhss_sync_controller_t *controller,
    const uint8_t *buffer,
    size_t buffer_length,
    int64_t rx_timestamp_us,
    fhss_core_rx_result_t *out_result
)
{
    if (controller == NULL || buffer == NULL || out_result == NULL ||
        rx_timestamp_us < (int64_t)controller->sync_offset_us) {
        return FHSS_CONTROLLER_STATUS_INVALID_ARG;
    }
    if (!controller->initialized) {
        return FHSS_CONTROLLER_STATUS_NOT_INITIALIZED;
    }

    /* Using the observed time as the expected time makes Core perform all
     * packet/hop validation while treating this bounded recovery sample as a
     * valid timing anchor. An invalid packet still cannot move the clock. */
    fhss_core_rx_result_t result = {0};
    if (fhss_core_process_rx(
            &controller->core,
            buffer,
            buffer_length,
            rx_timestamp_us,
            rx_timestamp_us,
            &result) != FHSS_CORE_STATUS_OK) {
        return FHSS_CONTROLLER_STATUS_CORE_ERROR;
    }

    const int64_t observed_slot_start_us =
        rx_timestamp_us - (int64_t)controller->sync_offset_us;
    if (fhss_slot_scheduler_set_reference(
            &controller->scheduler,
            result.packet.slot_number,
            observed_slot_start_us) != FHSS_SLOT_STATUS_OK) {
        return FHSS_CONTROLLER_STATUS_SCHEDULER_ERROR;
    }

    *out_result = result;
    return FHSS_CONTROLLER_STATUS_OK;
}
