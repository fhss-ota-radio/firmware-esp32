#include "fhss_timing_window.h"

fhss_timing_status_t fhss_timing_window_evaluate(
    const fhss_timing_window_config_t *config,
    int64_t expected_rx_time_us,
    int64_t actual_rx_time_us,
    fhss_timing_window_evaluation_t *out_evaluation
)
{
    /* 1. config 와 out_evaluation이 NULL인지 검사*/
    if (config == NULL || out_evaluation == NULL){
        return FHSS_TIMING_STATUS_INVALID_ARG;
    } 
    /* 2. timing_error_us 계산*/
    const int64_t timing_error_us = actual_rx_time_us - expected_rx_time_us;
   
    /* 허용 가능한 앞쪽, 뒤쪽 경계값 계산*/
    const int64_t early_limit_us = -(int64_t)config -> late_margin_us;
    const int64_t late_limit_us = (int64_t)config -> late_margin_us;
    /*계산 결과를 담을 임시 구조체 생성*/
    fhss_timing_window_evaluation_t evaluation = {
        .timing_error_us = timing_error_us,
        .result = FHSS_TIMING_INSIDE_WINDOW,
    };
    
   /* 5. 수신 시각이 윈도우 안에 있는지 판정*/
    if (timing_error_us < early_limit_us){
        evaluation.result = FHSS_TIMING_BEFORE_WINDOW;
    } else if (timing_error_us > late_limit_us) {
        evaluation.result = FHSS_TIMING_AFTER_WINDOW;
    } else {
        evaluation.result = FHSS_TIMING_INSIDE_WINDOW;
    }

    /* 6. 검증과 계산이 끝난 뒤 out_evaluation에 복사*/
    *out_evaluation = evaluation;

    /* 7. OK 반환*/
    return FHSS_TIMING_STATUS_OK;
}