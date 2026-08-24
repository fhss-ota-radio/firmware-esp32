#include "fhss_service.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "fhss_ota_diagnostics.h"

static const char *TAG = "fhss_service";

typedef enum {
    RECEIVE_RESULT_OK = 0,
    RECEIVE_RESULT_TIMEOUT,
    RECEIVE_RESULT_CRC_FAIL,
    RECEIVE_RESULT_RADIO_ERROR,
    /* [2026-08-24 추가] receive_one()의 rf_transport_receive_packet() 타임아웃
     * 전용 결과값. 예전엔 이것도 RECEIVE_RESULT_RADIO_ERROR로 묶여서
     * drain_rx_data_until()이 "진짜 하드웨어 오류"로 오인해 그 슬롯의 나머지
     * DATA를 통째로 포기했다 — 근데 실제로는 동기워드는 정상 감지됐고
     * (rf_transport_wait_rx_timestamp 성공) 그냥 본문 읽기가 빠듯했을 뿐이라
     * 하드웨어는 멀쩡하다. 이 결과는 TIMEOUT/CRC_FAIL과 같은 취급을 받아야
     * 한다 — drain 계속, tracking에서는 handle_miss()만. 자세한 재현 로그는
     * design-notes-gateway-ota-es.md 50절, notion-fhss-ack-channel-drop 문서
     * 참고. */
    RECEIVE_RESULT_BODY_TIMEOUT,
    RECEIVE_RESULT_SYNC_ERROR,
    RECEIVE_RESULT_DATA,
    RECEIVE_RESULT_SESSION_END,
} receive_result_t;

#define FHSS_SERVICE_TX_QUEUE_DEPTH 8U
/* Channel switching and SYNC transmission have hard slot deadlines. Keep the
 * radio service above temporary task-stats logging (priority 10) and audio
 * producer/consumer tasks (priority 3), which may run for several ms. */
#define FHSS_SERVICE_TASK_PRIORITY 12U
/* A maximum audio packet occupies roughly 13 ms on air at 38.4 kBaud once
 * CC1101 framing is included. Do not start DATA close enough to the next
 * channel-switch deadline that it can delay the following slot's SYNC. */
#define FHSS_SERVICE_DATA_TX_AIRTIME_GUARD_US 20000LL
/* Controlled A/B experiment hook. Enabling it makes the selected TRACKING
 * observations appear late without perturbing RF airtime. Keep it disabled
 * in normal firmware; the documented experiment used period 10. */
#define FHSS_SERVICE_TEST_DELAY_ENABLED 0U
#define FHSS_SERVICE_TEST_DELAY_PERIOD  10U
#define FHSS_SERVICE_TEST_DELAY_US     19000LL

typedef struct {
    uint8_t data[RF_TRANSPORT_MAX_PACKET_LENGTH];
    uint8_t length;
} fhss_service_tx_item_t;

static bool reset_controller(fhss_service_t *service)
{
    const fhss_sync_controller_config_t controller_config = {
        .core = {
            .channels = service->config.channels,
            .channel_count = service->config.channel_count,
            .hop_seed = service->config.hop_seed,
            .generation = service->config.generation,
            .reserved_channel = service->config.reserved_channel,
            .timing = {
                .early_margin_us = service->config.timing_window_margin_us,
                .late_margin_us = service->config.timing_window_margin_us,
            },
            .sync = {
                .acquire_count = service->config.acquire_count,
                .loss_count = service->config.loss_count,
            },
        },
        .scheduler = {
            .slot_duration_us = service->config.slot_duration_us,
            .channel_switch_guard_us =
                service->config.channel_switch_guard_us,
        },
        .sync_offset_us = service->config.sync_offset_us,
        .correction_deadband_us = service->config.correction_deadband_us,
        .correction_fast_threshold_us =
            service->config.correction_fast_threshold_us,
        .correction_slow_divisor =
            service->config.correction_slow_divisor,
        .correction_fast_divisor =
            service->config.correction_fast_divisor,
        .correction_max_step_us =
            service->config.correction_max_step_us,
    };
    const bool initialized = fhss_sync_controller_init(
                                 &service->controller,
                                 &controller_config) ==
                             FHSS_CONTROLLER_STATUS_OK;
    if (initialized) {
        service->test_tracking_sync_count = 0U;
    }
    return initialized;
}

static void diagnostics_lock(fhss_service_t *service)
{
    xSemaphoreTake((SemaphoreHandle_t)service->diagnostics_mutex, portMAX_DELAY);
}

static void diagnostics_unlock(fhss_service_t *service)
{
    xSemaphoreGive((SemaphoreHandle_t)service->diagnostics_mutex);
}

static void log_diagnostics(fhss_service_t *service, int64_t now_us)
{
    static bool csv_header_logged;
    fhss_diagnostics_snapshot_t snapshot = {0};
    if (!fhss_service_get_diagnostics(service, &snapshot)) {
        return;
    }

    const int64_t average_us = snapshot.timing_sample_count > 0U
        ? snapshot.timing_error_sum_us / snapshot.timing_sample_count
        : 0;
    const int64_t last_valid_age_ms = snapshot.last_valid_timestamp_us > 0
        ? (now_us - snapshot.last_valid_timestamp_us) / 1000
        : -1;
    const int64_t recovery_average_us =
        snapshot.recovery_duration_sample_count > 0U
        ? snapshot.recovery_duration_sum_us /
            snapshot.recovery_duration_sample_count
        : 0;
    const int64_t correction_average_us = snapshot.correction_applied_count > 0U
        ? snapshot.correction_abs_sum_us / snapshot.correction_applied_count
        : 0;
    ESP_LOGI(TAG,
             "DIAG state=%s valid=%lu crc_fail=%lu timeout=%lu "
             "acquired=%lu lost=%lu timing_us[min/avg/max]=%lld/%lld/%lld "
             "last_valid_age_ms=%lld",
             fhss_fsm_state_name(service->fsm.state),
             (unsigned long)snapshot.rx_valid_count,
             (unsigned long)snapshot.crc_fail_count,
             (unsigned long)snapshot.timeout_count,
             (unsigned long)snapshot.sync_acquired_count,
             (unsigned long)snapshot.sync_lost_count,
             (long long)snapshot.timing_error_min_us,
             (long long)average_us,
             (long long)snapshot.timing_error_max_us,
             (long long)last_valid_age_ms);

    ESP_LOGI(TAG,
             "DIAG recovery[entry/success/research]=%lu/%lu/%lu "
             "max_misses=%lu recovery_us[avg/max]=%lld/%lld "
             "correction[applied/avg_abs/max_abs]=%lu/%lld/%lld",
             (unsigned long)snapshot.recovery_entry_count,
             (unsigned long)snapshot.recovery_success_count,
             (unsigned long)snapshot.hard_research_count,
             (unsigned long)snapshot.max_consecutive_misses,
             (long long)recovery_average_us,
             (long long)snapshot.recovery_duration_max_us,
             (unsigned long)snapshot.correction_applied_count,
             (long long)correction_average_us,
             (long long)snapshot.correction_abs_max_us);

    /* Stable comma-separated output lets a long monitor capture be imported
     * directly into a spreadsheet for before/after algorithm comparison. */
    if (!csv_header_logged) {
        ESP_LOGI(TAG,
                 "FHSS_CSV_HEADER,uptime_ms,state,valid,crc_fail,timeout,"
                 "acquired,lost,timing_min_us,timing_avg_us,timing_max_us,"
                 "recovery_entry,recovery_success,hard_research,max_misses,"
                 "recovery_avg_us,recovery_max_us,correction_applied,"
                 "correction_avg_abs_us,correction_max_abs_us");
        csv_header_logged = true;
    }
    ESP_LOGI(TAG,
             "FHSS_CSV,%lld,%s,%lu,%lu,%lu,%lu,%lu,%lld,%lld,%lld,%lu,%lu,%lu,%lu,%lld,%lld,%lu,%lld,%lld",
             (long long)(now_us / 1000),
             fhss_fsm_state_name(service->fsm.state),
             (unsigned long)snapshot.rx_valid_count,
             (unsigned long)snapshot.crc_fail_count,
             (unsigned long)snapshot.timeout_count,
             (unsigned long)snapshot.sync_acquired_count,
             (unsigned long)snapshot.sync_lost_count,
             (long long)snapshot.timing_error_min_us,
             (long long)average_us,
             (long long)snapshot.timing_error_max_us,
             (unsigned long)snapshot.recovery_entry_count,
             (unsigned long)snapshot.recovery_success_count,
             (unsigned long)snapshot.hard_research_count,
             (unsigned long)snapshot.max_consecutive_misses,
             (long long)recovery_average_us,
             (long long)snapshot.recovery_duration_max_us,
             (unsigned long)snapshot.correction_applied_count,
             (long long)correction_average_us,
             (long long)snapshot.correction_abs_max_us);

    /* 재배정(2026-08-17): 채널이 3개일 땐 채널별 DIAG 3줄이 볼만했는데,
     * 150개로 늘면서 5초마다 150줄씩 찍혀 로그가 안 읽히는 수준이 됨.
     * valid/crc_fail/timeout이 전부 0인(아무 일도 없었던) 채널은 건너뛰고
     * 실제로 뭔가 있었던 채널만 남긴다. */
    for (size_t i = 0U; i < snapshot.channel_count; ++i) {
        const fhss_diagnostics_channel_t *channel = &snapshot.channels[i];
        if (channel->rx_valid_count == 0U &&
            channel->crc_fail_count == 0U &&
            channel->timeout_count == 0U) {
            continue;
        }
        ESP_LOGI(TAG, "DIAG channel=%u valid=%lu crc_fail=%lu timeout=%lu",
                 channel->channel,
                 (unsigned long)channel->rx_valid_count,
                 (unsigned long)channel->crc_fail_count,
                 (unsigned long)channel->timeout_count);
    }
}

static void maybe_log_diagnostics(
    fhss_service_t *service,
    int64_t *last_log_time_us
)
{
    if (service->config.diagnostics_interval_ms == 0U) {
        return;
    }
    const int64_t now_us = esp_timer_get_time();
    const int64_t interval_us =
        (int64_t)service->config.diagnostics_interval_ms * 1000;
    if (now_us - *last_log_time_us >= interval_us) {
        log_diagnostics(service, now_us);
        *last_log_time_us = now_us;
    }
}

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
    service->current_channel = channel;
    ESP_LOGI(TAG, "channel selected: slot=%lu channel=%u",
             (unsigned long)slot, channel);
    return true;
}

static rf_transport_status_t send_sync(
    fhss_service_t *service,
    uint32_t slot,
    uint16_t sequence
)
{
    uint8_t hop_index = 0U;
    if (fhss_hop_sequence_get_index(
            &service->controller.core.hop_sequence,
            slot,
            &hop_index) != FHSS_HOP_STATUS_OK) {
        return RF_TRANSPORT_STATUS_INVALID_ARG;
    }

    const fhss_sync_packet_t packet = {
        .version = FHSS_SYNC_PACKET_VERSION,
        .generation = service->config.generation,
        .sequence = sequence,
        .hop_index = hop_index,
        .slot_number = slot,
    };
    uint8_t buffer[FHSS_SYNC_PACKET_LENGTH] = {0};
    size_t length = 0U;
    if (fhss_sync_packet_encode(
            &packet, buffer, sizeof(buffer), &length) != FHSS_PACKET_STATUS_OK) {
        return RF_TRANSPORT_STATUS_INVALID_ARG;
    }
    fhss_ota_diag_log_packet(
        "TX", "FHSS_SYNC", service->current_channel, buffer, length);
    const rf_transport_status_t status = rf_transport_send_packet(
        &service->radio, buffer, (uint8_t)length);
    fhss_ota_diag_log_tx_result(
        "FHSS_SYNC", service->current_channel, (int)status, length);
    return status;
}

static void drain_tx_data_until(fhss_service_t *service, int64_t deadline_us)
{
    fhss_service_tx_item_t item;
    const int64_t latest_data_start_us =
        deadline_us - FHSS_SERVICE_DATA_TX_AIRTIME_GUARD_US;
    while (esp_timer_get_time() < latest_data_start_us) {
        const int64_t remaining_us =
            latest_data_start_us - esp_timer_get_time();
        TickType_t wait_ticks = pdMS_TO_TICKS(
            remaining_us > 20000 ? 20U : (uint32_t)(remaining_us / 1000));
        if (wait_ticks == 0U) {
            wait_ticks = 1U;
        }
        if (xQueueReceive(
                (QueueHandle_t)service->tx_queue,
                &item,
                wait_ticks) != pdTRUE) {
            continue;
        }
        if (esp_timer_get_time() >= latest_data_start_us) {
            /* Preserve the frame for the next slot. Dropping it here would
             * turn a scheduling guard into avoidable audio packet loss. */
            (void)xQueueSendToFront(
                (QueueHandle_t)service->tx_queue, &item, 0U);
            return;
        }
        service->tx_in_flight = true;
        fhss_ota_diag_log_packet(
            "TX", "FHSS_DATA", service->current_channel,
            item.data, item.length);
        const rf_transport_status_t send_status = rf_transport_send_packet(
            &service->radio, item.data, item.length);
        fhss_ota_diag_log_tx_result(
            "FHSS_DATA", service->current_channel,
            (int)send_status, item.length);
        if (send_status != RF_TRANSPORT_STATUS_OK) {
            service->tx_in_flight = false;
            ESP_LOGW(TAG, "audio/data TX failed: length=%u", item.length);
            report_event(service, FHSS_SERVICE_EVENT_ERROR);
            continue;
        }
        int64_t ignored_timestamp_us = 0;
        (void)rf_transport_wait_rx_timestamp(
            &service->radio, 20U, &ignored_timestamp_us);
        service->tx_in_flight = false;
    }
}

static void send_one_queued_data(fhss_service_t *service, int64_t deadline_us)
{
    if (esp_timer_get_time() >=
        deadline_us - FHSS_SERVICE_DATA_TX_AIRTIME_GUARD_US) {
        return;
    }
    fhss_service_tx_item_t item;
    if (xQueueReceive(
            (QueueHandle_t)service->tx_queue, &item, 0U) != pdTRUE) {
        return;
    }
    service->tx_in_flight = true;
    fhss_ota_diag_log_packet(
        "TX", "FHSS_RESPONSE", service->current_channel,
        item.data, item.length);
    const rf_transport_status_t send_status = rf_transport_send_packet(
        &service->radio, item.data, item.length);
    fhss_ota_diag_log_tx_result(
        "FHSS_RESPONSE", service->current_channel,
        (int)send_status, item.length);
    if (send_status != RF_TRANSPORT_STATUS_OK) {
        (void)xQueueSendToFront(
            (QueueHandle_t)service->tx_queue, &item, 0U);
        ESP_LOGW(TAG, "response TX failed: length=%u", item.length);
    }
    service->tx_in_flight = false;
    (void)rf_transport_start_receive(&service->radio);
}

static void tx_task(fhss_service_t *service)
{
    uint32_t slot = 0U;
    uint16_t sequence = 0U;
    int64_t slot_time_us = esp_timer_get_time() + 100000;
    fhss_slot_scheduler_set_reference(
        &service->controller.scheduler, slot, slot_time_us);

    for (;;) {
        if (service->should_stop) {
            return;
        }
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
        const rf_transport_status_t sync_tx_status =
            send_sync(service, slot, sequence);
        if (sync_tx_status != RF_TRANSPORT_STATUS_OK) {
            /* 반환 코드를 보존해야 SPI 오류와 CC1101 TX 완료 timeout을 로그만으로
             * 구분할 수 있다. 실패한 슬롯을 재시도하는 기존 동작은 우선 유지한다. */
            ESP_LOGE(TAG, "SYNC TX failed: slot=%lu status=%d",
                     (unsigned long)slot, sync_tx_status);
            report_event(service, FHSS_SERVICE_EVENT_ERROR);
            /* 같은 이미 지난 slot을 즉시 재시도하면 ERROR 큐만 폭주시킨다.
             * 상위 FSM이 ERROR 정리를 수행하도록 이 TX 세션 태스크를 끝낸다. */
            return;
        }

        int64_t tx_timestamp_us = 0;
        /* TX keeps its original slot clock. Re-anchoring here would add the
         * preamble/sync transmission latency to every slot. */
        (void)rf_transport_wait_rx_timestamp(
            &service->radio, 20U, &tx_timestamp_us);
        ESP_LOGI(TAG, "SYNC TX: slot=%lu seq=%u timestamp=%lld",
                 (unsigned long)slot, sequence, (long long)tx_timestamp_us);
        int64_t next_start_us = 0;
        if (fhss_slot_scheduler_get_slot_start_time(
                &service->controller.scheduler,
                slot + 1U,
                &next_start_us) == FHSS_SLOT_STATUS_OK) {
            drain_tx_data_until(
                service,
                next_start_us - service->config.channel_switch_guard_us);
        }
        slot++;
        sequence++;
    }
}

static receive_result_t receive_one(
    fhss_service_t *service,
    uint32_t timestamp_timeout_ms,
    uint32_t packet_timeout_ms,
    fhss_core_rx_result_t *out_result,
    int64_t *out_rx_timestamp_us
)
{
    if (rf_transport_start_receive(&service->radio) !=
        RF_TRANSPORT_STATUS_OK) {
        return RECEIVE_RESULT_RADIO_ERROR;
    }

    int64_t rx_timestamp_us = 0;
    if (rf_transport_wait_rx_timestamp(
            &service->radio, timestamp_timeout_ms, &rx_timestamp_us) !=
        RF_TRANSPORT_STATUS_OK) {
        return RECEIVE_RESULT_TIMEOUT;
    }

    rf_transport_rx_packet_t packet = {0};
    if (rf_transport_receive_packet(
            &service->radio,
            packet_timeout_ms,
            &packet) != RF_TRANSPORT_STATUS_OK) {
        /* [2026-08-24] 동기워드는 이미 잡았는데(위 wait_rx_timestamp 성공)
         * 본문을 다 못 읽고 여기서 타임아웃 — 하드웨어 오류가 아니라 그냥
         * 처리 시간이 빠듯했던 것뿐이므로 RADIO_ERROR와 구분한다. */
        return RECEIVE_RESULT_BODY_TIMEOUT;
    }
    fhss_ota_diag_log_rx_result(
        "FHSS", service->current_channel, 0, packet.crc_ok,
        packet.rssi_dbm, packet.lqi, packet.length);
    fhss_ota_diag_log_packet(
        "RX", "FHSS", service->current_channel,
        packet.payload, packet.length);
    if (!packet.crc_ok) {
        return RECEIVE_RESULT_CRC_FAIL;
    }

    if (!fhss_sync_packet_has_valid_magic(packet.payload, packet.length)) {
        if (service->fsm.state != FHSS_FSM_STATE_SEARCHING &&
            service->config.data_callback != NULL) {
            const fhss_service_data_action_t action =
                service->config.data_callback(
                packet.payload,
                packet.length,
                service->config.event_context);
            if (action == FHSS_SERVICE_DATA_SESSION_END) {
                *out_rx_timestamp_us = rx_timestamp_us;
                return RECEIVE_RESULT_SESSION_END;
            }
        }
        *out_rx_timestamp_us = rx_timestamp_us;
        return RECEIVE_RESULT_DATA;
    }

    int64_t controller_timestamp_us = rx_timestamp_us;
    if (FHSS_SERVICE_TEST_DELAY_ENABLED != 0U &&
        service->fsm.state == FHSS_FSM_STATE_TRACKING) {
        service->test_tracking_sync_count++;
        if ((service->test_tracking_sync_count %
             FHSS_SERVICE_TEST_DELAY_PERIOD) == 0U) {
            controller_timestamp_us += FHSS_SERVICE_TEST_DELAY_US;
            ESP_LOGW(TAG,
                     "A/B FAULT: sync_sample=%lu injected_delay=%lld us",
                     (unsigned long)service->test_tracking_sync_count,
                     (long long)FHSS_SERVICE_TEST_DELAY_US);
        }
    }

    const fhss_sync_controller_status_t sync_status =
        service->fsm.state == FHSS_FSM_STATE_RECOVERY
            ? fhss_sync_controller_recover_rx(
                &service->controller,
                packet.payload,
                packet.length,
                controller_timestamp_us,
                out_result)
            : fhss_sync_controller_process_rx(
                &service->controller,
                packet.payload,
                packet.length,
                controller_timestamp_us,
                out_result);
    if (sync_status != FHSS_CONTROLLER_STATUS_OK) {
        return RECEIVE_RESULT_SYNC_ERROR;
    }

    ESP_LOGI(TAG,
             "SYNC RX: state=%s slot=%lu channel=%u error=%lld us "
             "correction=%lld us accumulated=%lld us timestamp=%lld",
             fhss_fsm_state_name(service->fsm.state),
             (unsigned long)out_result->packet.slot_number,
             out_result->channel,
             (long long)out_result->timing.timing_error_us,
             (long long)service->controller.last_phase_correction_us,
             (long long)service->controller.accumulated_phase_correction_us,
             (long long)rx_timestamp_us);
    *out_rx_timestamp_us = rx_timestamp_us;
    return RECEIVE_RESULT_OK;
}

static void record_receive_result(
    fhss_service_t *service,
    receive_result_t receive_result,
    const fhss_core_rx_result_t *result,
    int64_t rx_timestamp_us
)
{
    diagnostics_lock(service);
    if (receive_result == RECEIVE_RESULT_OK) {
        fhss_diagnostics_record_valid(
            &service->diagnostics,
            result->channel,
            result->timing.timing_error_us,
            rx_timestamp_us);
        fhss_diagnostics_record_correction(
            &service->diagnostics,
            service->controller.last_phase_correction_us);
    } else if (receive_result == RECEIVE_RESULT_CRC_FAIL) {
        fhss_diagnostics_record_crc_fail(
            &service->diagnostics, service->current_channel);
    } else if (receive_result == RECEIVE_RESULT_TIMEOUT ||
               receive_result == RECEIVE_RESULT_BODY_TIMEOUT) {
        fhss_diagnostics_record_timeout(
            &service->diagnostics, service->current_channel);
    }
    diagnostics_unlock(service);
}

static void handle_sync_result(
    fhss_service_t *service,
    const fhss_core_rx_result_t *result
)
{
    service->consecutive_sync_misses = 0U;
    service->recovery_probe_index = 0U;
    if (service->fsm.state == FHSS_FSM_STATE_RECOVERY) {
        ESP_LOGI(TAG,
                 "RECOVERY succeeded: slot=%lu channel=%u; tracking resumed",
                 (unsigned long)result->packet.slot_number,
                 result->channel);
        diagnostics_lock(service);
        fhss_diagnostics_record_recovery_success(
            &service->diagnostics, esp_timer_get_time());
        diagnostics_unlock(service);
        fhss_fsm_handle(&service->fsm, FHSS_FSM_EVENT_SYNC_RECOVERED);
    }
    if (service->fsm.state == FHSS_FSM_STATE_SEARCHING &&
        result->timing.result == FHSS_TIMING_INSIDE_WINDOW) {
        fhss_fsm_handle(&service->fsm, FHSS_FSM_EVENT_FIRST_SYNC);
    }
    if (result->sync_event == FHSS_SYNC_EVENT_ACQUIRED) {
        diagnostics_lock(service);
        fhss_diagnostics_record_sync_acquired(&service->diagnostics);
        diagnostics_unlock(service);
        fhss_fsm_handle(&service->fsm, FHSS_FSM_EVENT_SYNC_ACQUIRED);
        report_event(service, FHSS_SERVICE_EVENT_SYNC_ACQUIRED);
    }
    if (result->sync_event == FHSS_SYNC_EVENT_LOST) {
        diagnostics_lock(service);
        fhss_diagnostics_record_sync_lost(&service->diagnostics);
        fhss_diagnostics_record_hard_research(&service->diagnostics);
        diagnostics_unlock(service);
        fhss_slot_scheduler_clear_reference(&service->controller.scheduler);
        fhss_fsm_handle(&service->fsm, FHSS_FSM_EVENT_SYNC_LOST);
        report_event(service, FHSS_SERVICE_EVENT_SYNC_LOST);
    }
}

static void handle_miss(fhss_service_t *service)
{
    const bool was_synchronizing =
        service->fsm.state == FHSS_FSM_STATE_SYNCHRONIZING;
    service->consecutive_sync_misses++;
    diagnostics_lock(service);
    fhss_diagnostics_record_miss(
        &service->diagnostics, service->consecutive_sync_misses);
    diagnostics_unlock(service);
    fhss_sync_event_t event = FHSS_SYNC_EVENT_NONE;
    fhss_sync_state_t state = FHSS_SYNC_STATE_SEARCHING;
    if (fhss_sync_controller_handle_timeout(
            &service->controller, &event, &state) !=
        FHSS_CONTROLLER_STATUS_OK) {
        report_event(service, FHSS_SERVICE_EVENT_ERROR);
        return;
    }
    if (event == FHSS_SYNC_EVENT_LOST) {
        diagnostics_lock(service);
        fhss_diagnostics_record_sync_lost(&service->diagnostics);
        fhss_diagnostics_record_hard_research(&service->diagnostics);
        diagnostics_unlock(service);
        fhss_fsm_handle(&service->fsm, FHSS_FSM_EVENT_SYNC_LOST);
        report_event(service, FHSS_SERVICE_EVENT_SYNC_LOST);
        service->consecutive_sync_misses = 0U;
        service->recovery_probe_index = 0U;
    } else if (was_synchronizing) {
        fhss_slot_scheduler_clear_reference(&service->controller.scheduler);
        fhss_fsm_handle(&service->fsm, FHSS_FSM_EVENT_SYNC_LOST);
        service->consecutive_sync_misses = 0U;
    } else if (service->fsm.state == FHSS_FSM_STATE_TRACKING &&
               service->consecutive_sync_misses >=
                   service->config.recovery_entry_miss_count) {
        /* Keep the scheduler reference. RECOVERY probes nearby slot channels
         * before the hard loss_count threshold is allowed to clear it. */
        fhss_fsm_handle(&service->fsm, FHSS_FSM_EVENT_SYNC_DEGRADED);
        service->recovery_probe_index = 0U;
        diagnostics_lock(service);
        fhss_diagnostics_record_recovery_entry(
            &service->diagnostics, esp_timer_get_time());
        diagnostics_unlock(service);
        ESP_LOGW(TAG,
                 "RECOVERY entered after %lu consecutive sync misses",
                 (unsigned long)service->consecutive_sync_misses);
    }
}

static void handle_invalid_sync(fhss_service_t *service)
{
    ESP_LOGW(TAG, "invalid SYNC dropped: state=%s channel=%u",
             fhss_fsm_state_name(service->fsm.state),
             service->current_channel);

    /* During acquisition, a malformed or incompatible peer packet must not
     * erase already validated samples and must not escalate to product ERROR.
     * Once tracking, the same packet means the expected valid SYNC was missed,
     * so feed it into the normal bounded recovery/loss policy. */
    if (service->fsm.state == FHSS_FSM_STATE_TRACKING ||
        service->fsm.state == FHSS_FSM_STATE_RECOVERY) {
        handle_miss(service);
    }
}

static uint32_t get_recovery_probe_slot(
    fhss_service_t *service,
    uint32_t predicted_slot
)
{
    /* Probe the predicted channel first, then one slot behind and one ahead.
     * Repeating this bounded pattern covers the common +/- one-slot slip while
     * the original scheduler clock continues to advance. */
    static const int8_t offsets[] = {0, -1, 1};
    const int8_t offset = offsets[
        service->recovery_probe_index % (sizeof(offsets) / sizeof(offsets[0]))];
    service->recovery_probe_index++;

    if (offset < 0 && predicted_slot == 0U) {
        return predicted_slot;
    }
    return offset < 0
        ? predicted_slot - 1U
        : predicted_slot + (uint32_t)offset;
}

static bool handle_session_end(fhss_service_t *service)
{
    /* A peer ended the talkspurt normally. Clear the old timing reference and
     * return to the common rendezvous channel without counting fake misses or
     * escalating the application through SYNC_LOST. */
    fhss_fsm_init(&service->fsm);
    if (!reset_controller(service) ||
        !fhss_fsm_handle(&service->fsm, FHSS_FSM_EVENT_START_RX)) {
        report_event(service, FHSS_SERVICE_EVENT_ERROR);
        return false;
    }
    service->consecutive_sync_misses = 0U;
    service->recovery_probe_index = 0U;
    service->test_tracking_sync_count = 0U;
    if (!select_channel(service, 0U)) {
        report_event(service, FHSS_SERVICE_EVENT_ERROR);
        return false;
    }
    ESP_LOGI(TAG, "peer talkspurt ended; rendezvous RX resumed on channel=%u",
             service->current_channel);
    return true;
}

static bool drain_rx_data_until(
    fhss_service_t *service,
    int64_t switch_time_us
)
{
    /* SYNC is sent at the slot boundary and audio packets follow on the same
     * channel. Keep receiving those DATA packets until the channel-switch
     * guard instead of sleeping through the usable part of the slot. */
    for (;;) {
        const int64_t remaining_us = switch_time_us - esp_timer_get_time();
        if (remaining_us <= 1000) {
            return false;
        }

        /* A slave may need to return OTA ACK/NACK on the current hop channel.
         * Serialize those responses in this radio-owning task instead of
         * letting ota_client touch the CC1101 concurrently. */
        send_one_queued_data(service, switch_time_us);

        /* 재배정(2026-08-17): 20ms 상한이 49바이트 페이로드(38.4kBaud 기준
         * 순수 전송시간만 약 10~13ms) + GDO0 감지~태스크 기상 지연 + SPI
         * 폴링 오버헤드를 감당하기엔 너무 빠듯해서, sync word는 정상
         * 감지됐는데(rf_transport_wait_rx_timestamp 성공) 본문을 다 못 읽고
         * rf_transport_receive_packet()이 타임아웃 나는 경우가 실기기에서
         * 거의 매 슬롯("RX data drain stopped: result=3(RADIO_ERROR)")
         * 반복 확인됨 — 이게 RECEIVE_RESULT_RADIO_ERROR로 분류돼 드레인
         * 루프가 그 슬롯 데이터를 통째로 포기하고 있었음. 40ms로 올려도
         * 위 remaining_us 캡 때문에 슬롯 경계(switch_time_us)를 넘기진
         * 않음 — 슬롯 뒷부분(전환 임박)에서는 remaining_us 자체가 이 상한보다
         * 먼저 작아져서 자연히 더 짧게 잡힌다.
         *
         * [2026-08-24 후속] 이 타임아웃이 여기서 드레인을 끊어버리는 사이
         * DATA 처리 + ACK 송신이 밀려서, ACK가 DATA를 받은 채널이 아니라
         * 다음 홉 채널에서 나가 Gateway가 못 듣는 문제로 이어졌다(실기기
         * 재현: design-notes-gateway-ota-es.md 50절). 근본 수정은
         * RECEIVE_RESULT_RADIO_ERROR를 "진짜 무선 오류"와 "본문 읽기
         * 타임아웃"(RECEIVE_RESULT_BODY_TIMEOUT)으로 분리해서, 후자는
         * TIMEOUT/CRC_FAIL처럼 드레인을 계속하도록 함 — 아래 분기 참고. */
        uint32_t timeout_ms = (uint32_t)((remaining_us + 999) / 1000);
        if (timeout_ms > 40U) {
            timeout_ms = 40U;
        }

        fhss_core_rx_result_t result = {0};
        int64_t rx_timestamp_us = 0;
        const receive_result_t receive_result = receive_one(
            service,
            timeout_ms,
            timeout_ms,
            &result,
            &rx_timestamp_us);
        record_receive_result(
            service, receive_result, &result, rx_timestamp_us);

        if (receive_result == RECEIVE_RESULT_DATA) {
            continue;
        }
        if (receive_result == RECEIVE_RESULT_SESSION_END) {
            return handle_session_end(service);
        }
        if (receive_result == RECEIVE_RESULT_OK) {
            /* Tolerate a repeated SYNC while draining without losing the
             * opportunity to refresh the scheduler reference. */
            handle_sync_result(service, &result);
            continue;
        }
        if (receive_result == RECEIVE_RESULT_TIMEOUT ||
            receive_result == RECEIVE_RESULT_CRC_FAIL) {
            /* Optional DATA absence is not a missed synchronization slot. */
            continue;
        }
        if (receive_result == RECEIVE_RESULT_BODY_TIMEOUT) {
            /* [2026-08-24] 동기워드는 잡았지만 본문 읽기가 빠듯했던 경우 —
             * 진짜 무선 오류가 아니므로 예전처럼 이 슬롯 전체를 포기하지
             * 않고 남은 시간 동안 드레인을 계속한다. 이게 바로 DATA 뒤에
             * 오는 ACK가 다음 홉 채널로 밀려서 유실되던 원인이었다
             * (design-notes 50절 / notion-fhss-ack-channel-drop 문서). */
            continue;
        }

        ESP_LOGW(TAG, "RX data drain stopped: result=%d channel=%u",
                 receive_result, service->current_channel);
        return false;
    }
}

static void rx_task(fhss_service_t *service)
{
    /* 재배정(2026-08-17): 랑데부 채널 고정 방식으로 변경. 예전엔 SEARCHING이
     * scan_slot을 계속 늘리며 channels[] 전체(지금은 150개)를 137ms씩 훑어
     * TX와 우연히 같은 채널·같은 순간에 걸리길 기다렸다 — 채널이 3개일 땐
     * 버틸 만했지만 150개로 늘리면서 최악 20초+ 걸리고, TX 300ms/RX 137ms
     * 주기의 위상 관계에 따라 특정 채널 조합은 거의 못 만나는 문제가 있었다.
     * hop_channels[0](=CHANNR 0)을 고정 랑데부 채널로 삼아 SEARCHING
     * 동안은 항상 그 채널만 듣는다 — TX도 매 세션 slot=0부터 시작하므로
     * (tx_task 참고) 첫 SYNC는 항상 이 채널에서 나가 자연히 같은 곳에서
     * 만난다. 일단 만나면(FIRST_SYNC -> SYNCHRONIZING -> TRACKING) 기존
     * 슬롯 스케줄러가 SYNC 패킷의 slot_number로 두 기기를 같은 홉 순서에
     * 태우므로, 이후 150채널 홉은 그대로 유지된다 — 채널 수 확장의 영향은
     * "초기 만남"이 아니라 "만난 뒤 홉 폭"에만 미친다. */
    int64_t last_diagnostics_log_us = esp_timer_get_time();
    for (;;) {
        if (service->should_stop) {
            return;
        }
        maybe_log_diagnostics(service, &last_diagnostics_log_us);
        if (service->fsm.state == FHSS_FSM_STATE_SEARCHING) {
            if (!select_channel(service, 0U)) {
                report_event(service, FHSS_SERVICE_EVENT_ERROR);
                vTaskDelay(pdMS_TO_TICKS(100U));
                continue;
            }
            fhss_core_rx_result_t result = {0};
            int64_t rx_timestamp_us = 0;
            const receive_result_t receive_result = receive_one(
                service,
                service->config.search_dwell_ms,
                service->config.receive_timeout_ms,
                &result,
                &rx_timestamp_us);
            record_receive_result(
                service, receive_result, &result, rx_timestamp_us);
            if (receive_result == RECEIVE_RESULT_OK) {
                handle_sync_result(service, &result);
            } else if (receive_result == RECEIVE_RESULT_DATA) {
                /* Ignore data until a SYNC reference has been acquired. */
            } else if (receive_result == RECEIVE_RESULT_RADIO_ERROR ||
                       receive_result == RECEIVE_RESULT_SYNC_ERROR) {
                ESP_LOGW(TAG, "RX processing error: result=%d channel=%u",
                         receive_result, service->current_channel);
            } else if (receive_result == RECEIVE_RESULT_BODY_TIMEOUT) {
                /* SEARCHING 단계에서도 진짜 오류가 아니므로 조용히 다음
                 * 시도로 넘어간다 (드레인 루프와 동일한 취급). */
            }
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

        if (drain_rx_data_until(service, switch_time_us)) {
            continue;
        }
        delay_until_us(switch_time_us);
        uint32_t channel_slot = next_slot;
        if (service->fsm.state == FHSS_FSM_STATE_RECOVERY) {
            channel_slot = get_recovery_probe_slot(service, next_slot);
            ESP_LOGI(TAG,
                     "RECOVERY probe: predicted_slot=%lu candidate_slot=%lu",
                     (unsigned long)next_slot,
                     (unsigned long)channel_slot);
        }
        if (!select_channel(service, channel_slot)) {
            report_event(service, FHSS_SERVICE_EVENT_ERROR);
            continue;
        }

        fhss_core_rx_result_t result = {0};
        int64_t rx_timestamp_us = 0;
        const receive_result_t receive_result = receive_one(
            service,
            service->config.receive_timeout_ms,
            service->config.receive_timeout_ms,
            &result,
            &rx_timestamp_us);
        record_receive_result(
            service, receive_result, &result, rx_timestamp_us);
        if (receive_result == RECEIVE_RESULT_OK) {
            handle_sync_result(service, &result);
        } else if (receive_result == RECEIVE_RESULT_SESSION_END) {
            (void)handle_session_end(service);
        } else if (receive_result == RECEIVE_RESULT_DATA) {
            /* Data packets do not advance the sync miss counter. */
        } else if (receive_result == RECEIVE_RESULT_TIMEOUT ||
                   receive_result == RECEIVE_RESULT_CRC_FAIL ||
                   receive_result == RECEIVE_RESULT_BODY_TIMEOUT) {
            /* [2026-08-24] BODY_TIMEOUT을 TIMEOUT/CRC_FAIL과 같은 취급으로
             * 옮김 — 예전엔 RECEIVE_RESULT_RADIO_ERROR로 묶여서 아래
             * report_event(FHSS_SERVICE_EVENT_ERROR)까지 타면서 그냥
             * 처리시간이 빠듯했던 것뿐인데 심각한 오류로 취급됐었다. */
            handle_miss(service);
        } else if (receive_result == RECEIVE_RESULT_SYNC_ERROR) {
            handle_invalid_sync(service);
        } else if (receive_result == RECEIVE_RESULT_RADIO_ERROR) {
            ESP_LOGE(TAG, "CC1101 RX failure on channel=%u",
                     service->current_channel);
            report_event(service, FHSS_SERVICE_EVENT_ERROR);
        } else {
            ESP_LOGW(TAG, "RX processing error: result=%d channel=%u",
                     receive_result, service->current_channel);
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
    /* 태스크가 오류로 스스로 끝나는 경우에도 다음 역할 전환/start가 stale
     * handle을 살아 있는 태스크로 오인하지 않도록 먼저 NULL로 공개한다. */
    service->task_handle = NULL;
    vTaskDelete(NULL);
}

bool fhss_service_init(
    fhss_service_t *service,
    const fhss_service_config_t *config
)
{
    if (service == NULL || config == NULL || config->channels == NULL ||
        config->channel_count == 0U || config->slot_duration_us == 0U ||
        config->search_dwell_ms == 0U || config->receive_timeout_ms == 0U ||
        config->correction_slow_divisor == 0U ||
        config->correction_fast_divisor == 0U ||
        config->correction_max_step_us == 0U ||
        config->correction_deadband_us >
            config->correction_fast_threshold_us ||
        config->recovery_entry_miss_count == 0U ||
        config->recovery_entry_miss_count >= config->loss_count) {
        return false;
    }

    memset(service, 0, sizeof(*service));
    service->config = *config;
    fhss_fsm_init(&service->fsm);

    if (!fhss_diagnostics_init(
            &service->diagnostics, config->channels, config->channel_count)) {
        return false;
    }

    if (rf_transport_init(&service->radio, &config->radio) !=
        RF_TRANSPORT_STATUS_OK) {
        return false;
    }
    /* 실제 성공했던 단독 smoke test와 동일하게, reset/configure 명령을 보내기
     * 전에 PARTNUM/VERSION을 먼저 읽는다. 이 값과 이후 설정 read-back을 나눠
     * 기록하면 최초 SPI 응답 문제와 설정 단계 문제를 구분할 수 있다. */
    rf_transport_chip_info_t chip_info = {0};
    if (rf_transport_read_chip_info(&service->radio, &chip_info) !=
        RF_TRANSPORT_STATUS_OK) {
        ESP_LOGE(TAG, "CC1101 chip-info read failed during service init");
        return false;
    }
    ESP_LOGI(TAG, "CC1101 pre-config PARTNUM=0x%02X VERSION=0x%02X",
             chip_info.partnum, chip_info.version);
    if (chip_info.partnum != 0x00U || chip_info.version == 0xFFU) {
        ESP_LOGE(TAG, "invalid CC1101 identity; check MISO/power/CS wiring");
        return false;
    }
    if (chip_info.version == 0x00U) {
        ESP_LOGW(TAG,
                 "CC1101 pre-config VERSION=0x00; continuing to register read-back test");
    }

    if (rf_transport_configure_433mhz(&service->radio) !=
        RF_TRANSPORT_STATUS_OK) {
        return false;
    }

    if (!reset_controller(service)) {
        return false;
    }

    service->diagnostics_mutex = xSemaphoreCreateMutex();
    if (service->diagnostics_mutex == NULL) {
        return false;
    }
    service->tx_queue = xQueueCreate(
        FHSS_SERVICE_TX_QUEUE_DEPTH, sizeof(fhss_service_tx_item_t));
    if (service->tx_queue == NULL) {
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

    /* 재배정(2026-08-17): FHSS_DIAGNOSTICS_MAX_CHANNELS를 150채널에 맞춰
     * 16->160으로 올리면서 log_diagnostics()의 스택 지역변수
     * fhss_diagnostics_snapshot_t가 ~2.6KB로 커짐 — 기존 4096바이트
     * 스택으론 ESP_LOGI 포맷팅 등 나머지 호출 체인과 합쳐 스택 오버플로우
     * 위험이 있어 여유 있게 올림. */
    TaskHandle_t task = NULL;
    if (xTaskCreate(
            service_task,
            "fhss_service",
            8192U,
            service,
            FHSS_SERVICE_TASK_PRIORITY,
            &task) != pdPASS) {
        fhss_fsm_handle(&service->fsm, FHSS_FSM_EVENT_STOP);
        return false;
    }
    service->task_handle = task;
    return true;
}

bool fhss_service_pause(fhss_service_t *service)
{
    if (service == NULL || !service->initialized) {
        return false;
    }
    if (service->task_handle != NULL) {
        service->should_stop = true;
        while (service->task_handle != NULL) {
            vTaskDelay(pdMS_TO_TICKS(5U));
        }
        service->should_stop = false;
    }
    service->tx_in_flight = false;
    if (service->tx_queue != NULL) {
        xQueueReset((QueueHandle_t)service->tx_queue);
    }
    fhss_fsm_handle(&service->fsm, FHSS_FSM_EVENT_STOP);
    if (rf_transport_recover_433mhz(&service->radio) !=
        RF_TRANSPORT_STATUS_OK) {
        ESP_LOGE(TAG, "CC1101 recovery failed while pausing service");
        return false;
    }
    return true;
}

bool fhss_service_set_role(
    fhss_service_t *service,
    fhss_service_role_t role
)
{
    if (service == NULL || !service->initialized ||
        (role != FHSS_SERVICE_ROLE_TX && role != FHSS_SERVICE_ROLE_RX)) {
        return false;
    }
    if (service->config.role == role && service->task_handle != NULL) {
        return true;
    }

    /* 재배정(2026-08-17): 예전엔 여기서 task_handle을 vTaskDelete()로 바로
     * 죽였다 — 아래 rf_transport_recover_433mhz()가 "CS LOW/HIGH 사이에서
     * 끊길 수 있다"는 걸 이미 알고 복구를 시도하고는 있었지만, 짧은 PTT
     * 세션(약 1.2초)에서 tx_task가 SPI 전송 한복판에 죽어 SPI 버스/CC1101이
     * 잠긴 채로 남고, 그 뒤에 호출되는 rf_transport_recover_433mhz() 자체도
     * 잠긴 SPI에 물려 무한 대기하는 전체 행(hang)이 실기기에서 재현됨 —
     * fsm_task가 여기서 멈추니 로터리엔코더/PTT 입력도 같이 죽었음.
     * tx_audio_task/rx_audio_task에 이미 쓰던 협조적 종료(should_stop 플래그
     * + 태스크 스스로 종료 대기)로 교체 — 최악 응답 지연은 슬롯 1개(~300ms)
     * 남짓이라 강제 종료 없이도 충분히 빠르다. */
    if (service->task_handle != NULL) {
        service->should_stop = true;
        while (service->task_handle != NULL) {
            vTaskDelay(pdMS_TO_TICKS(5U));
        }
        service->should_stop = false;
    }
    /* task가 SPI 전송 중간이 아니라 루프 시작 지점에서만 빠져나오므로 이제는
     * 항상 CS HIGH(유휴) 상태에서 멈춘다 — 그래도 역할 전환 시 레지스터를
     * 다시 정렬해두는 것은 안전하니 유지한다. */
    if (rf_transport_recover_433mhz(&service->radio) !=
        RF_TRANSPORT_STATUS_OK) {
        ESP_LOGE(TAG, "CC1101 recovery failed while switching role");
        return false;
    }
    fhss_fsm_init(&service->fsm);
    if (!reset_controller(service)) {
        return false;
    }
    service->config.role = role;
    service->tx_in_flight = false;
    /* A role change starts a new radio session. Do not let stale RX recovery
     * progress force the new session into RECOVERY or SEARCHING early. */
    service->consecutive_sync_misses = 0U;
    service->recovery_probe_index = 0U;
    if (role == FHSS_SERVICE_ROLE_RX) {
        xQueueReset((QueueHandle_t)service->tx_queue);
    }
    return fhss_service_start(service);
}

bool fhss_service_send_data(
    fhss_service_t *service,
    const uint8_t *data,
    size_t length
)
{
    if (service == NULL || data == NULL || length == 0U ||
        length > RF_TRANSPORT_MAX_PACKET_LENGTH ||
        service->tx_queue == NULL) {
        return false;
    }
    fhss_service_tx_item_t item = { .length = (uint8_t)length };
    memcpy(item.data, data, length);
    return xQueueSend((QueueHandle_t)service->tx_queue, &item, 0U) == pdTRUE;
}

bool fhss_service_wait_tx_idle(
    fhss_service_t *service,
    uint32_t timeout_ms
)
{
    if (service == NULL || !service->initialized ||
        service->config.role != FHSS_SERVICE_ROLE_TX ||
        service->tx_queue == NULL) {
        return false;
    }

    const TickType_t start_ticks = xTaskGetTickCount();
    const TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);
    do {
        if (uxQueueMessagesWaiting((QueueHandle_t)service->tx_queue) == 0U &&
            !service->tx_in_flight) {
            return true;
        }
        vTaskDelay(1U);
    } while ((xTaskGetTickCount() - start_ticks) < timeout_ticks);

    return uxQueueMessagesWaiting((QueueHandle_t)service->tx_queue) == 0U &&
           !service->tx_in_flight;
}

fhss_fsm_state_t fhss_service_get_state(const fhss_service_t *service)
{
    return service != NULL ? service->fsm.state : FHSS_FSM_STATE_STOPPED;
}

bool fhss_service_get_diagnostics(
    fhss_service_t *service,
    fhss_diagnostics_snapshot_t *out_snapshot
)
{
    if (service == NULL || out_snapshot == NULL ||
        service->diagnostics_mutex == NULL) {
        return false;
    }
    diagnostics_lock(service);
    *out_snapshot = service->diagnostics;
    diagnostics_unlock(service);
    return true;
}
