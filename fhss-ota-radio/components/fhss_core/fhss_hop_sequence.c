#include "fhss_hop_sequence.h"

#include <stdbool.h>
#include <limits.h>

static uint32_t next_random(uint32_t *state)
{
    /* xorshift32 is intentionally deterministic: peers using the same seed
     * build the same channel permutation without exchanging the full table.
     * It is a hopping-order generator, not a cryptographic primitive. */
    uint32_t value = *state;
    value ^= value << 13U;
    value ^= value >> 17U;
    value ^= value << 5U;
    *state = value;
    return value;
}

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

static fhss_hop_status_t initialize_sequence(
    fhss_hop_sequence_t *sequence,
    const uint8_t *channels,
    size_t channel_count,
    uint32_t seed,
    uint8_t reserved_channel,
    bool enforce_reserved_channel
)
{
    if (sequence == NULL || channels == NULL) {
        return FHSS_HOP_STATUS_INVALID_ARG;
    }
    if (channel_count == 0U || channel_count > (size_t)UINT8_MAX + 1U) {
        return FHSS_HOP_STATUS_INVALID_CONFIG;
    }

    fhss_hop_sequence_t initialized_sequence = {
        .channels = channels,
        .channel_count = channel_count,
        .seed = seed,
        .initialized = 1U,
    };

    for (size_t i = 0U; i < channel_count; ++i) {
        if (enforce_reserved_channel && channels[i] == reserved_channel) {
            return FHSS_HOP_STATUS_INVALID_CONFIG;
        }
        for (size_t j = 0U; j < i; ++j) {
            if (channels[i] == channels[j]) {
                return FHSS_HOP_STATUS_INVALID_CONFIG;
            }
        }
        initialized_sequence.permutation[i] = (uint8_t)i;
    }

    /* Keep the first configured channel as the rendezvous channel for slot 0.
     * Shuffle only the remaining entries so initial discovery stays stable. */
    uint32_t random_state = seed != 0U ? seed : 0x6D2B79F5U;
    for (size_t i = channel_count; i > 2U; --i) {
        const size_t selected = 1U +
            (size_t)(next_random(&random_state) % (uint32_t)(i - 1U));
        const uint8_t temporary = initialized_sequence.permutation[i - 1U];
        initialized_sequence.permutation[i - 1U] =
            initialized_sequence.permutation[selected];
        initialized_sequence.permutation[selected] = temporary;
    }

    *sequence = initialized_sequence;
    return FHSS_HOP_STATUS_OK;
}

fhss_hop_status_t fhss_hop_sequence_init(fhss_hop_sequence_t *sequence,
                                          const uint8_t *channels,
                                          size_t channel_count)
{
    /* Preserve the original API: it has no reserved-channel policy. */
    return initialize_sequence(
        sequence, channels, channel_count, 1U, 0U, false);
}

fhss_hop_status_t fhss_hop_sequence_init_seeded(
    fhss_hop_sequence_t *sequence,
    const uint8_t *channels,
    size_t channel_count,
    uint32_t seed,
    uint8_t reserved_channel
)
{
    return initialize_sequence(
        sequence, channels, channel_count, seed, reserved_channel, true);
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

    const size_t position = slot_number % sequence->channel_count;
    *out_index = sequence->permutation[position];
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
