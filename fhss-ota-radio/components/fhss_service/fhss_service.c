#include "fhss_service.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "fhss_service";

static void report_event(fhss_service_t *service, fhss_service_event_t event)
{
    if (service->config.event_callback != NULL) {
        service->config.event_callback(event, service->config.event_context);
    }
}

static void delay_until_us(int64_t target_us)
{
    for (;;) {
        const int64_t remaining_us = target_us - esp_timer_get_time();
        if (remaining_us <= 0) {
            return;
        }
        if (remaining_us > 2000) {
            TickType_t ticks = pdMS_TO_TICKS((uint32_t)(remaining_us / 1000));
            if (ticks > 1U) {
                vTaskDelay(ticks - 1U);
            } else {
                taskYIELD();
            }
        } else {
            taskYIELD();
        }
    }
}

static bool select_channel(fhss_service_t *service, uint32_t slot)
{
    uint8_t channel = 0U;
    if (fhss_core_get_channel(&service->controller.core, slot, &channel) !=
        FHSS_CORE_STATUS_OK) {
        return false;
    }
    if (rf_transport_set_channel(&service->radio, channel) !=
        RF_TRANSPORT_STATUS_OK) {
        return false;
    }
    ESP_LOGI(TAG, "channel selected: slot=%lu channel=%u",
             (unsigned long)slot, channel);
    return true;
}

static bool send_sync(fhss_service_t *service, uint32_t slot, uint16_t sequence)
{
    uint8_t hop_index = 0U;
    if (fhss_hop_sequence_get_index(
            &service->controller.core.hop_sequence,
            slot,
            &hop_index) != FHSS_HOP_STATUS_OK) {
        return false;
    }

    const fhss_sync_packet_t packet = {
        .version = FHSS_SYNC_PACKET_VERSION,
        .type = FHSS_PACKET_TYPE_SYNC,
        .sequence = sequence,
        .hop_index = hop_index,
        .slot_number = slot,
    };
    uint8_t buffer[FHSS_SYNC_PACKET_LENGTH] = {0};
    size_t length = 0U;
    if (fhss_sync_packet_encode(
            &packet, buffer, sizeof(buffer), &length) != FHSS_PACKET_STATUS_OK) {
        return false;
    }
    return rf_transport_send_packet(&service->radio, buffer, (uint8_t)length) ==
        RF_TRANSPORT_STATUS_OK;
}

static void tx_task(fhss_service_t *service)
{
    uint32_t slot = 0U;
    uint16_t sequence = 0U;
    int64_t slot_time_us = esp_timer_get_time() + 100000;
    fhss_slot_scheduler_set_reference(
        &service->controller.scheduler, slot, slot_time_us);

    for (;;) {
        int64_t start_us = 0;
        if (fhss_slot_scheduler_get_slot_start_time(
                &service->controller.scheduler, slot, &start_us) !=
            FHSS_SLOT_STATUS_OK) {
            report_event(service, FHSS_SERVICE_EVENT_ERROR);
            vTaskDelay(pdMS_TO_TICKS(100U));
            continue;
        }

        const int64_t switch_us =
            start_us - service->config.channel_switch_guard_us;
        delay_until_us(switch_us);
        if (!select_channel(service, slot)) {
            report_event(service, FHSS_SERVICE_EVENT_ERROR);
            continue;
        }

        delay_until_us(start_us);
        if (!send_sync(service, slot, sequence)) {
            ESP_LOGE(TAG, "SYNC TX failed: slot=%lu", (unsigned long)slot);
            report_event(service, FHSS_SERVICE_EVENT_ERROR);
            continue;
        }

        int64_t tx_timestamp_us = 0;
        /* TX keeps its original slot clock. Re-anchoring here would add the
         * preamble/sync transmission latency to every slot. */
        (void)rf_transport_wait_rx_timestamp(
            &service->radio, 20U, &tx_timestamp_us);
        ESP_LOGI(TAG, "SYNC TX: slot=%lu seq=%u timestamp=%lld",
                 (unsigned long)slot, sequence, (long long)tx_timestamp_us);
        slot++;
        sequence++;
    }
}

static bool receive_one(
    fhss_service_t *service,
    uint32_t timestamp_timeout_ms,
    fhss_core_rx_result_t *out_result
)
{
    if (rf_transport_start_receive(&service->radio) !=
        RF_TRANSPORT_STATUS_OK) {
        return false;
    }

    int64_t rx_timestamp_us = 0;
    if (rf_transport_wait_rx_timestamp(
            &service->radio, timestamp_timeout_ms, &rx_timestamp_us) !=
        RF_TRANSPORT_STATUS_OK) {
        return false;
    }

    rf_transport_rx_packet_t packet = {0};
    if (rf_transport_receive_packet(
            &service->radio,
            service->config.receive_timeout_ms,
            &packet) != RF_TRANSPORT_STATUS_OK || !packet.crc_ok) {
        return false;
    }

    if (fhss_sync_controller_process_rx(
            &service->controller,
            packet.payload,
            packet.length,
            rx_timestamp_us,
            out_result) != FHSS_CONTROLLER_STATUS_OK) {
        return false;
    }

    ESP_LOGI(TAG,
             "SYNC RX: state=%s slot=%lu channel=%u error=%lld us timestamp=%lld",
             fhss_fsm_state_name(service->fsm.state),
             (unsigned long)out_result->packet.slot_number,
             out_result->channel,
             (long long)out_result->timing.timing_error_us,
             (long long)rx_timestamp_us);
    return true;
}

static void handle_sync_result(
    fhss_service_t *service,
    const fhss_core_rx_result_t *result
)
{
    if (service->fsm.state == FHSS_FSM_STATE_SEARCHING &&
        result->timing.result == FHSS_TIMING_INSIDE_WINDOW) {
        fhss_fsm_handle(&service->fsm, FHSS_FSM_EVENT_FIRST_SYNC);
    }
    if (result->sync_event == FHSS_SYNC_EVENT_ACQUIRED) {
        fhss_fsm_handle(&service->fsm, FHSS_FSM_EVENT_SYNC_ACQUIRED);
        report_event(service, FHSS_SERVICE_EVENT_SYNC_ACQUIRED);
    }
    if (result->sync_event == FHSS_SYNC_EVENT_LOST) {
        fhss_slot_scheduler_clear_reference(&service->controller.scheduler);
        fhss_fsm_handle(&service->fsm, FHSS_FSM_EVENT_SYNC_LOST);
        report_event(service, FHSS_SERVICE_EVENT_SYNC_LOST);
    }
}

static void handle_miss(fhss_service_t *service)
{
    const bool was_synchronizing =
        service->fsm.state == FHSS_FSM_STATE_SYNCHRONIZING;
    fhss_sync_event_t event = FHSS_SYNC_EVENT_NONE;
    fhss_sync_state_t state = FHSS_SYNC_STATE_SEARCHING;
    if (fhss_sync_controller_handle_timeout(
            &service->controller, &event, &state) !=
        FHSS_CONTROLLER_STATUS_OK) {
        report_event(service, FHSS_SERVICE_EVENT_ERROR);
        return;
    }
    if (event == FHSS_SYNC_EVENT_LOST) {
        fhss_fsm_handle(&service->fsm, FHSS_FSM_EVENT_SYNC_LOST);
        report_event(service, FHSS_SERVICE_EVENT_SYNC_LOST);
    } else if (was_synchronizing) {
        fhss_slot_scheduler_clear_reference(&service->controller.scheduler);
        fhss_fsm_handle(&service->fsm, FHSS_FSM_EVENT_SYNC_LOST);
    }
}

static void rx_task(fhss_service_t *service)
{
    uint32_t scan_slot = 0U;
    for (;;) {
        if (service->fsm.state == FHSS_FSM_STATE_SEARCHING) {
            if (!select_channel(service, scan_slot)) {
                report_event(service, FHSS_SERVICE_EVENT_ERROR);
                vTaskDelay(pdMS_TO_TICKS(100U));
                continue;
            }
            fhss_core_rx_result_t result = {0};
            if (receive_one(service, service->config.search_dwell_ms, &result)) {
                handle_sync_result(service, &result);
            }
            scan_slot++;
            continue;
        }

        const int64_t now_us = esp_timer_get_time();
        uint32_t next_slot = 0U;
        int64_t switch_time_us = 0;
        if (fhss_slot_scheduler_get_next_switch_time(
                &service->controller.scheduler,
                now_us,
                &next_slot,
                &switch_time_us) != FHSS_SLOT_STATUS_OK) {
            fhss_fsm_handle(&service->fsm, FHSS_FSM_EVENT_SYNC_LOST);
            fhss_slot_scheduler_clear_reference(&service->controller.scheduler);
            continue;
        }

        delay_until_us(switch_time_us);
        if (!select_channel(service, next_slot)) {
            report_event(service, FHSS_SERVICE_EVENT_ERROR);
            continue;
        }

        fhss_core_rx_result_t result = {0};
        if (receive_one(service, service->config.receive_timeout_ms, &result)) {
            handle_sync_result(service, &result);
        } else {
            handle_miss(service);
        }
    }
}

static void service_task(void *arg)
{
    fhss_service_t *service = (fhss_service_t *)arg;
    if (service->config.role == FHSS_SERVICE_ROLE_TX) {
        tx_task(service);
    } else {
        rx_task(service);
    }
    vTaskDelete(NULL);
}

bool fhss_service_init(
    fhss_service_t *service,
    const fhss_service_config_t *config
)
{
    if (service == NULL || config == NULL || config->channels == NULL ||
        config->channel_count == 0U || config->slot_duration_us == 0U ||
        config->search_dwell_ms == 0U || config->receive_timeout_ms == 0U) {
        return false;
    }

    memset(service, 0, sizeof(*service));
    service->config = *config;
    fhss_fsm_init(&service->fsm);

    if (rf_transport_init(&service->radio, &config->radio) !=
        RF_TRANSPORT_STATUS_OK) {
        return false;
    }
    if (rf_transport_configure_433mhz(&service->radio) !=
        RF_TRANSPORT_STATUS_OK) {
        return false;
    }

    const fhss_sync_controller_config_t controller_config = {
        .core = {
            .channels = config->channels,
            .channel_count = config->channel_count,
            .timing = {
                .early_margin_us = config->channel_switch_guard_us,
                .late_margin_us = config->channel_switch_guard_us,
            },
            .sync = {
                .acquire_count = config->acquire_count,
                .loss_count = config->loss_count,
            },
        },
        .scheduler = {
            .slot_duration_us = config->slot_duration_us,
            .channel_switch_guard_us = config->channel_switch_guard_us,
        },
        .sync_offset_us = config->sync_offset_us,
    };
    if (fhss_sync_controller_init(
            &service->controller,
            &controller_config) != FHSS_CONTROLLER_STATUS_OK) {
        return false;
    }

    service->initialized = true;
    return true;
}

bool fhss_service_start(fhss_service_t *service)
{
    if (service == NULL || !service->initialized || service->task_handle != NULL) {
        return false;
    }

    fhss_fsm_handle(
        &service->fsm,
        service->config.role == FHSS_SERVICE_ROLE_TX
            ? FHSS_FSM_EVENT_START_TX
            : FHSS_FSM_EVENT_START_RX
    );

    TaskHandle_t task = NULL;
    if (xTaskCreate(
            service_task,
            "fhss_service",
            4096U,
            service,
            6U,
            &task) != pdPASS) {
        fhss_fsm_handle(&service->fsm, FHSS_FSM_EVENT_STOP);
        return false;
    }
    service->task_handle = task;
    return true;
}

fhss_fsm_state_t fhss_service_get_state(const fhss_service_t *service)
{
    return service != NULL ? service->fsm.state : FHSS_FSM_STATE_STOPPED;
}
