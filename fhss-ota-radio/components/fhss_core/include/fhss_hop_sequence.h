#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FHSS_HOP_STATUS_OK = 0,
    FHSS_HOP_STATUS_INVALID_ARG,
    FHSS_HOP_STATUS_INVALID_CONFIG,
    FHSS_HOP_STATUS_NOT_INITIALIZED,
} fhss_hop_status_t;

typedef struct {
    const uint8_t *channels;
    size_t channel_count;
    uint8_t initialized;
} fhss_hop_sequence_t;

fhss_hop_status_t fhss_hop_sequence_init(fhss_hop_sequence_t *sequence,
                                          const uint8_t *channels,
                                          size_t channel_count);
fhss_hop_status_t fhss_hop_sequence_get_index(const fhss_hop_sequence_t *sequence,
                                               uint32_t slot_number,
                                               uint8_t *out_index);
fhss_hop_status_t fhss_hop_sequence_get_channel(const fhss_hop_sequence_t *sequence,
                                                 uint32_t slot_number,
                                                 uint8_t *out_channel);

#ifdef __cplusplus
}
#endif
