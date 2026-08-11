#include "fhss_hop_sequence.h"

#include <limits.h>

static fhss_hop_status_t validate_sequence(const fhss_hop_sequence_t *sequence)
{
    if (sequence == NULL) {
        return FHSS_HOP_STATUS_INVALID_ARG;
    }
    if (sequence->initialized == 0U) {
        return FHSS_HOP_STATUS_NOT_INITIALIZED;
    }
    return FHSS_HOP_STATUS_OK;
}

fhss_hop_status_t fhss_hop_sequence_init(fhss_hop_sequence_t *sequence,
                                          const uint8_t *channels,
                                          size_t channel_count)
{
    if (sequence == NULL || channels == NULL) {
        return FHSS_HOP_STATUS_INVALID_ARG;
    }
    if (channel_count == 0U || channel_count > (size_t)UINT8_MAX + 1U) {
        return FHSS_HOP_STATUS_INVALID_CONFIG;
    }

    const fhss_hop_sequence_t initialized_sequence = {
        .channels = channels,
        .channel_count = channel_count,
        .initialized = 1U,
    };
    *sequence = initialized_sequence;
    return FHSS_HOP_STATUS_OK;
}

fhss_hop_status_t fhss_hop_sequence_get_index(const fhss_hop_sequence_t *sequence,
                                               uint32_t slot_number,
                                               uint8_t *out_index)
{
    if (out_index == NULL) {
        return FHSS_HOP_STATUS_INVALID_ARG;
    }
    const fhss_hop_status_t status = validate_sequence(sequence);
    if (status != FHSS_HOP_STATUS_OK) {
        return status;
    }

    *out_index = (uint8_t)(slot_number % sequence->channel_count);
    return FHSS_HOP_STATUS_OK;
}

fhss_hop_status_t fhss_hop_sequence_get_channel(const fhss_hop_sequence_t *sequence,
                                                 uint32_t slot_number,
                                                 uint8_t *out_channel)
{
    if (out_channel == NULL) {
        return FHSS_HOP_STATUS_INVALID_ARG;
    }

    uint8_t index = 0U;
    const fhss_hop_status_t status =
        fhss_hop_sequence_get_index(sequence, slot_number, &index);
    if (status != FHSS_HOP_STATUS_OK) {
        return status;
    }

    *out_channel = sequence->channels[index];
    return FHSS_HOP_STATUS_OK;
}
