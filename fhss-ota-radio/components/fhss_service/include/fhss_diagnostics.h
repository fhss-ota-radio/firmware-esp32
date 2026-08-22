#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* 재배정(2026-08-17): 호핑 채널을 150개로 늘리면서 예전 상한(16)에 걸려
 * fhss_diagnostics_init()이 조용히(로그 없이) 실패 -> fhss_service_init()
 * 전체가 CC1101 통신 시도 전에 실패하는 문제가 있었음. 실제 사용 채널 수(150)
 * 이상으로 올림. */
#define FHSS_DIAGNOSTICS_MAX_CHANNELS 160U

typedef struct {
    uint8_t channel;
    uint32_t rx_valid_count;
    uint32_t crc_fail_count;
    uint32_t timeout_count;
} fhss_diagnostics_channel_t;

typedef struct {
    fhss_diagnostics_channel_t channels[FHSS_DIAGNOSTICS_MAX_CHANNELS];
    size_t channel_count;
    uint32_t rx_valid_count;
    uint32_t crc_fail_count;
    uint32_t timeout_count;
    uint32_t sync_acquired_count;
    uint32_t sync_lost_count;
    uint32_t timing_sample_count;
    int64_t timing_error_min_us;
    int64_t timing_error_max_us;
    int64_t timing_error_sum_us;
    int64_t last_valid_timestamp_us;
    uint32_t recovery_entry_count;
    uint32_t recovery_success_count;
    uint32_t hard_research_count;
    uint32_t max_consecutive_misses;
    uint32_t recovery_duration_sample_count;
    int64_t recovery_duration_sum_us;
    int64_t recovery_duration_max_us;
    uint32_t correction_applied_count;
    int64_t correction_abs_sum_us;
    int64_t correction_abs_max_us;
    int64_t recovery_started_timestamp_us;
} fhss_diagnostics_t;

typedef fhss_diagnostics_t fhss_diagnostics_snapshot_t;

bool fhss_diagnostics_init(
    fhss_diagnostics_t *diagnostics,
    const uint8_t *channels,
    size_t channel_count
);

void fhss_diagnostics_record_valid(
    fhss_diagnostics_t *diagnostics,
    uint8_t channel,
    int64_t timing_error_us,
    int64_t timestamp_us
);
void fhss_diagnostics_record_crc_fail(
    fhss_diagnostics_t *diagnostics,
    uint8_t channel
);
void fhss_diagnostics_record_timeout(
    fhss_diagnostics_t *diagnostics,
    uint8_t channel
);
void fhss_diagnostics_record_sync_acquired(fhss_diagnostics_t *diagnostics);
void fhss_diagnostics_record_sync_lost(fhss_diagnostics_t *diagnostics);
void fhss_diagnostics_record_miss(
    fhss_diagnostics_t *diagnostics,
    uint32_t consecutive_misses
);
void fhss_diagnostics_record_recovery_entry(
    fhss_diagnostics_t *diagnostics,
    int64_t timestamp_us
);
void fhss_diagnostics_record_recovery_success(
    fhss_diagnostics_t *diagnostics,
    int64_t timestamp_us
);
void fhss_diagnostics_record_hard_research(fhss_diagnostics_t *diagnostics);
void fhss_diagnostics_record_correction(
    fhss_diagnostics_t *diagnostics,
    int64_t correction_us
);

