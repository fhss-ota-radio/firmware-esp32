#pragma once

#include <stddef.h>
#include <stdint.h>

#include "rf_transport.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FHSS_AUDIO_PACKET_MAGIC             0xA5U
#define FHSS_AUDIO_PACKET_VERSION           1U
#define FHSS_AUDIO_PACKET_TYPE              0x02U
#define FHSS_AUDIO_PACKET_MAX_FRAMES        2U
#define FHSS_AUDIO_PACKET_HEADER_SIZE       9U
#define FHSS_AUDIO_PACKET_MAX_PAYLOAD_BYTES \
    (RF_TRANSPORT_MAX_PACKET_LENGTH - FHSS_AUDIO_PACKET_HEADER_SIZE)

#define FHSS_AUDIO_PACKET_FLAG_END_OF_TALKSPURT 0x01U

typedef enum {
    FHSS_AUDIO_PACKET_STATUS_OK = 0,
    FHSS_AUDIO_PACKET_STATUS_INVALID_ARGUMENT,
    FHSS_AUDIO_PACKET_STATUS_BUFFER_TOO_SMALL,
    FHSS_AUDIO_PACKET_STATUS_PAYLOAD_TOO_LARGE,
    FHSS_AUDIO_PACKET_STATUS_INVALID_FORMAT,
} fhss_audio_packet_status_t;

typedef struct {
    const uint8_t *data;
    size_t length;
} fhss_audio_frame_view_t;

typedef struct {
    uint16_t sequence;
    uint8_t flags;
    uint8_t frame_count;
    fhss_audio_frame_view_t frames[FHSS_AUDIO_PACKET_MAX_FRAMES];
} fhss_audio_packet_view_t;

fhss_audio_packet_status_t fhss_audio_packet_pack(
    uint16_t sequence,
    uint8_t flags,
    const fhss_audio_frame_view_t *frames,
    size_t frame_count,
    uint8_t *out_packet,
    size_t out_capacity,
    size_t *out_length
);

/*
 * Decoding is zero-copy. Frame data points into packet and remains valid only
 * while the caller-owned packet buffer remains valid and unchanged.
 */
fhss_audio_packet_status_t fhss_audio_packet_unpack(
    const uint8_t *packet,
    size_t packet_length,
    fhss_audio_packet_view_t *out_view
);

#ifdef __cplusplus
}
#endif
