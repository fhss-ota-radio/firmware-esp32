#include "fhss_service.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/queue.h"

static const char *TAG = "fhss_service";

typedef enum {
    RECEIVE_RESULT_OK = 0,
    RECEIVE_RESULT_TIMEOUT,
    RECEIVE_RESULT_CRC_FAIL,
    RECEIVE_RESULT_RADIO_ERROR,
    RECEIVE_RESULT_SYNC_ERROR,
    RECEIVE_RESULT_DATA,
} receive_result_t;

#define FHSS_SERVICE_TX_QUEUE_DEPTH 8U

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
            .timing = {
                .early_margin_us = service->config.channel_switch_guard_us,
                .late_margin_us = service->config.channel_switch_guard_us,
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
    };
    return fhss_sync_controller_init(
               &service->controller,
               &controller_config) == FHSS_CONTROLLER_STATUS_OK;
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

    for (size_t i = 0U; i < snapshot.channel_count; ++i) {
        const fhss_diagnostics_channel_t *channel = &snapshot.channels[i];
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
        .type = FHSS_PACKET_TYPE_SYNC,
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
    return rf_transport_send_packet(&service->radio, buffer, (uint8_t)length);
}

static void drain_tx_data_until(fhss_service_t *service, int64_t deadline_us)
{
    fhss_service_tx_item_t item;
    while (esp_timer_get_time() < deadline_us) {
        const int64_t remaining_us = deadline_us - esp_timer_get_time();
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
        if (esp_timer_get_time() >= deadline_us) {
            (void)xQueueSendToFront(
                (QueueHandle_t)service->tx_queue, &item, 0U);
            return;
        }
        service->tx_in_flight = true;
        if (rf_transport_send_packet(&service->radio, item.data, item.length) !=
            RF_TRANSPORT_STATUS_OK) {
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
        return RECEIVE_RESULT_RADIO_ERROR;
    }
    if (!packet.crc_ok) {
        return RECEIVE_RESULT_CRC_FAIL;
    }

    if (!fhss_sync_packet_has_valid_magic(packet.payload, packet.length)) {
        if (service->fsm.state != FHSS_FSM_STATE_SEARCHING &&
            service->config.data_callback != NULL) {
            service->config.data_callback(
                packet.payload,
                packet.length,
                service->config.event_context);
        }
        *out_rx_timestamp_us = rx_timestamp_us;
        return RECEIVE_RESULT_DATA;
    }

    if (fhss_sync_controller_process_rx(
            &service->controller,
            packet.payload,
            packet.length,
            rx_timestamp_us,
            out_result) != FHSS_CONTROLLER_STATUS_OK) {
        return RECEIVE_RESULT_SYNC_ERROR;
    }

    ESP_LOGI(TAG,
             "SYNC RX: state=%s slot=%lu channel=%u error=%lld us timestamp=%lld",
             fhss_fsm_state_name(service->fsm.state),
             (unsigned long)out_result->packet.slot_number,
             out_result->channel,
             (long long)out_result->timing.timing_error_us,
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
    } else if (receive_result == RECEIVE_RESULT_CRC_FAIL) {
        fhss_diagnostics_record_crc_fail(
            &service->diagnostics, service->current_channel);
    } else if (receive_result == RECEIVE_RESULT_TIMEOUT) {
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
        diagnostics_unlock(service);
        fhss_fsm_handle(&service->fsm, FHSS_FSM_EVENT_SYNC_LOST);
        report_event(service, FHSS_SERVICE_EVENT_SYNC_LOST);
    } else if (was_synchronizing) {
        fhss_slot_scheduler_clear_reference(&service->controller.scheduler);
        fhss_fsm_handle(&service->fsm, FHSS_FSM_EVENT_SYNC_LOST);
    }
}

static void drain_rx_data_until(
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
            return;
        }

        uint32_t timeout_ms = (uint32_t)((remaining_us + 999) / 1000);
        if (timeout_ms > 20U) {
            timeout_ms = 20U;
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

        ESP_LOGW(TAG, "RX data drain stopped: result=%d channel=%u",
                 receive_result, service->current_channel);
        return;
    }
}

static void rx_task(fhss_service_t *service)
{
    uint32_t scan_slot = 0U;
    int64_t last_diagnostics_log_us = esp_timer_get_time();
    for (;;) {
        maybe_log_diagnostics(service, &last_diagnostics_log_us);
        if (service->fsm.state == FHSS_FSM_STATE_SEARCHING) {
            if (!select_channel(service, scan_slot)) {
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

        drain_rx_data_until(service, switch_time_us);
        delay_until_us(switch_time_us);
        if (!select_channel(service, next_slot)) {
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
        } else if (receive_result == RECEIVE_RESULT_DATA) {
            /* Data packets do not advance the sync miss counter. */
        } else if (receive_result == RECEIVE_RESULT_TIMEOUT ||
                   receive_result == RECEIVE_RESULT_CRC_FAIL) {
            handle_miss(service);
        } else {
            ESP_LOGW(TAG, "RX processing error: result=%d channel=%u",
                     receive_result, service->current_channel);
            report_event(service, FHSS_SERVICE_EVENT_ERROR);
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
        config->search_dwell_ms == 0U || config->receive_timeout_ms == 0U) {
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

    if (service->task_handle != NULL) {
        vTaskDelete((TaskHandle_t)service->task_handle);
        service->task_handle = NULL;
    }
    /* vTaskDelete can interrupt the old RX task between CS LOW and CS HIGH.
     * Recover SPI framing and radio registers before the new role starts. */
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
        service->config.role != FHSS_SERVICE_ROLE_TX ||
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
