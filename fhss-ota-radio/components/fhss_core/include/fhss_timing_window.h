#pragma once

#include <stdint.h>

typedef enum {
    FHSS_TIMING_STATUS_OK = 0, 
    FHSS_TIMING_STATUS_INVALID_ARG,
} fhss_timing_status_t;

typedef enum {
    FHSS_TIMING_BEFORE_WINDOW = 0, 
    FHSS_TIMING_INSIDE_WINDOW, 
    FHSS_TIMING_AFTER_WINDOW,
} fhss_timing_window_result_t;

typedef struct {
    uint32_t early_margin_us; //예상 시간보다 얼마나 일직 와도 허용할지
    uint32_t late_margin_us; //예상 시간보다 얼마나 늦게 와도 허용할지
} fhss_timing_window_config_t;

typedef struct {
    int64_t timing_error_us; // 실제 수신 시각 - 예상 수신 시각
    fhss_timing_window_result_t result;
} fhss_timing_window_evaluation_t;

fhss_timing_status_t fhss_timing_window_evaluate(
    const fhss_timing_window_config_t *config,
    int64_t expected_rx_time_us,
    int64_t actual_rx_time_us,
    fhss_timing_window_evaluation_t *out_evaluation
);