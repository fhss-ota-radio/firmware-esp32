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

typedef void (*fhss_service_data_callback_t)(
    const uint8_t *data,
    size_t length,
    void *context
);

typedef struct {
    fhss_service_role_t role;
    rf_transport_config_t radio;
    const uint8_t *channels;
    size_t channel_count;
    uint32_t slot_duration_us;
    uint32_t channel_switch_guard_us;
    /* 재배정(2026-08-17): 예전엔 channel_switch_guard_us(5ms, 채널 전환용
     * 리드타임)를 수신 타이밍 판정 허용 오차로도 그대로 재사용했는데, 이
     * 둘은 완전히 다른 예산이다 — 채널 전환은 SPI/CC1101 처리시간만 확보하면
     * 되지만, 판정 오차는 GDO0 ISR 지연/FreeRTOS 스케줄링 지터/잔여 클럭
     * 드리프트까지 다 흡수해야 한다. 5ms는 이 지터 예산으론 타이트해서
     * 실제로 패킷은 정상 수신됐는데 타이밍만 창을 벗어나 MISS로
     * 판정되는(수신자가 RX를 놓치는) 사례가 실기기에서 확인됨 — 별도
     * 필드로 분리해 더 넉넉하게 잡는다. */
    uint32_t timing_window_margin_us;
    uint32_t sync_offset_us;
    uint32_t search_dwell_ms;
    uint32_t receive_timeout_ms;
    uint32_t acquire_count;
    uint32_t loss_count;
    uint32_t diagnostics_interval_ms;
    fhss_service_event_callback_t event_callback;
    fhss_service_data_callback_t data_callback;
    void *event_context;
} fhss_service_config_t;

typedef struct {
    fhss_service_config_t config;
    rf_transport_t radio;
    fhss_sync_controller_t controller;
    fhss_fsm_t fsm;
    fhss_diagnostics_t diagnostics;
    void *diagnostics_mutex;
    void *tx_queue;
    void *task_handle;
    uint8_t current_channel;
    volatile bool tx_in_flight;
    /* 재배정(2026-08-17): fhss_service_set_role()이 이전엔 task_handle을
     * vTaskDelete()로 직접 강제 종료했는데, tx_task/rx_task가 SPI 전송
     * 중간(CS 로우 구간 등)에 죽으면 CC1101/SPI 버스가 잠긴 상태로 남아
     * 이후 모든 SPI 호출이 무한 대기하는 전체 행(hang)이 실기기에서
     * 확인됨(짧은 PTT 세션에서 재현). tx_audio_task/rx_audio_task에 이미
     * 쓰던 것과 같은 협조적 종료 플래그로 교체. */
    volatile bool should_stop;
    bool initialized;
} fhss_service_t;

bool fhss_service_init(
    fhss_service_t *service,
    const fhss_service_config_t *config
);

bool fhss_service_start(fhss_service_t *service);
bool fhss_service_set_role(
    fhss_service_t *service,
    fhss_service_role_t role
);
bool fhss_service_send_data(
    fhss_service_t *service,
    const uint8_t *data,
    size_t length
);
bool fhss_service_wait_tx_idle(
    fhss_service_t *service,
    uint32_t timeout_ms
);
fhss_fsm_state_t fhss_service_get_state(const fhss_service_t *service);
bool fhss_service_get_diagnostics(
    fhss_service_t *service,
    fhss_diagnostics_snapshot_t *out_snapshot
);
