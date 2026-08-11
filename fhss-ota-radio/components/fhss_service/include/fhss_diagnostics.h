#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FHSS_DIAGNOSTICS_MAX_CHANNELS 16U

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

