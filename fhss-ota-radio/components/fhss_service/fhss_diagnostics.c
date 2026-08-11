#include "fhss_diagnostics.h"

#include <string.h>

static fhss_diagnostics_channel_t *find_channel(
    fhss_diagnostics_t *diagnostics,
    uint8_t channel
)
{
    for (size_t i = 0U; i < diagnostics->channel_count; ++i) {
        if (diagnostics->channels[i].channel == channel) {
            return &diagnostics->channels[i];
        }
    }
    return NULL;
}

bool fhss_diagnostics_init(
    fhss_diagnostics_t *diagnostics,
    const uint8_t *channels,
    size_t channel_count
)
{
    if (diagnostics == NULL || channels == NULL || channel_count == 0U ||
        channel_count > FHSS_DIAGNOSTICS_MAX_CHANNELS) {
        return false;
    }

    memset(diagnostics, 0, sizeof(*diagnostics));
    diagnostics->channel_count = channel_count;
    for (size_t i = 0U; i < channel_count; ++i) {
        diagnostics->channels[i].channel = channels[i];
    }
    return true;
}

void fhss_diagnostics_record_valid(
    fhss_diagnostics_t *diagnostics,
    uint8_t channel,
    int64_t timing_error_us,
    int64_t timestamp_us
)
{
    if (diagnostics == NULL) {
        return;
    }

    fhss_diagnostics_channel_t *channel_stats =
        find_channel(diagnostics, channel);
    diagnostics->rx_valid_count++;
    if (channel_stats != NULL) {
        channel_stats->rx_valid_count++;
    }

    if (diagnostics->timing_sample_count == 0U) {
        diagnostics->timing_error_min_us = timing_error_us;
        diagnostics->timing_error_max_us = timing_error_us;
    } else {
        if (timing_error_us < diagnostics->timing_error_min_us) {
            diagnostics->timing_error_min_us = timing_error_us;
        }
        if (timing_error_us > diagnostics->timing_error_max_us) {
            diagnostics->timing_error_max_us = timing_error_us;
        }
    }
    diagnostics->timing_error_sum_us += timing_error_us;
    diagnostics->timing_sample_count++;
    diagnostics->last_valid_timestamp_us = timestamp_us;
}

void fhss_diagnostics_record_crc_fail(
    fhss_diagnostics_t *diagnostics,
    uint8_t channel
)
{
    if (diagnostics == NULL) {
        return;
    }
    diagnostics->crc_fail_count++;
    fhss_diagnostics_channel_t *channel_stats =
        find_channel(diagnostics, channel);
    if (channel_stats != NULL) {
        channel_stats->crc_fail_count++;
    }
}

void fhss_diagnostics_record_timeout(
    fhss_diagnostics_t *diagnostics,
    uint8_t channel
)
{
    if (diagnostics == NULL) {
        return;
    }
    diagnostics->timeout_count++;
    fhss_diagnostics_channel_t *channel_stats =
        find_channel(diagnostics, channel);
    if (channel_stats != NULL) {
        channel_stats->timeout_count++;
    }
}

void fhss_diagnostics_record_sync_acquired(fhss_diagnostics_t *diagnostics)
{
    if (diagnostics != NULL) {
        diagnostics->sync_acquired_count++;
    }
}

void fhss_diagnostics_record_sync_lost(fhss_diagnostics_t *diagnostics)
{
    if (diagnostics != NULL) {
        diagnostics->sync_lost_count++;
    }
}

