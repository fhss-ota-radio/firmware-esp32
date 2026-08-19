#include "fhss_audio_packet.h"

#include <string.h>

enum {
    OFFSET_MAGIC = 0,
    OFFSET_VERSION = 1,
    OFFSET_TYPE = 2,
    OFFSET_FLAGS = 3,
    OFFSET_FRAME_COUNT = 4,
    OFFSET_SEQUENCE_LO = 5,
    OFFSET_SEQUENCE_HI = 6,
    OFFSET_FRAME_0_LENGTH = 7,
    OFFSET_FRAME_1_LENGTH = 8,
};

static int flags_are_valid(uint8_t flags)
{
    return (flags & (uint8_t)~FHSS_AUDIO_PACKET_FLAG_END_OF_TALKSPURT) == 0U;
}

fhss_audio_packet_status_t fhss_audio_packet_pack(
    uint16_t sequence,
    uint8_t flags,
    const fhss_audio_frame_view_t *frames,
    size_t frame_count,
    uint8_t *out_packet,
    size_t out_capacity,
    size_t *out_length
)
{
    if (frames == NULL || out_packet == NULL || out_length == NULL ||
        frame_count == 0U || frame_count > FHSS_AUDIO_PACKET_MAX_FRAMES ||
        !flags_are_valid(flags)) {
        return FHSS_AUDIO_PACKET_STATUS_INVALID_ARGUMENT;
    }

    size_t payload_length = 0U;
    for (size_t i = 0U; i < frame_count; ++i) {
        if (frames[i].data == NULL || frames[i].length == 0U ||
            frames[i].length > UINT8_MAX) {
            return FHSS_AUDIO_PACKET_STATUS_INVALID_ARGUMENT;
        }
        if (frames[i].length >
            FHSS_AUDIO_PACKET_MAX_PAYLOAD_BYTES - payload_length) {
            return FHSS_AUDIO_PACKET_STATUS_PAYLOAD_TOO_LARGE;
        }
        payload_length += frames[i].length;
    }

    const size_t packet_length = FHSS_AUDIO_PACKET_HEADER_SIZE + payload_length;
    if (out_capacity < packet_length) {
        return FHSS_AUDIO_PACKET_STATUS_BUFFER_TOO_SMALL;
    }

    uint8_t packet[RF_TRANSPORT_MAX_PACKET_LENGTH] = {0};
    packet[OFFSET_MAGIC] = FHSS_AUDIO_PACKET_MAGIC;
    packet[OFFSET_VERSION] = FHSS_AUDIO_PACKET_VERSION;
    packet[OFFSET_TYPE] = FHSS_AUDIO_PACKET_TYPE;
    packet[OFFSET_FLAGS] = flags;
    packet[OFFSET_FRAME_COUNT] = (uint8_t)frame_count;
    packet[OFFSET_SEQUENCE_LO] = (uint8_t)(sequence & 0xFFU);
    packet[OFFSET_SEQUENCE_HI] = (uint8_t)(sequence >> 8U);

    size_t payload_offset = FHSS_AUDIO_PACKET_HEADER_SIZE;
    for (size_t i = 0U; i < frame_count; ++i) {
        packet[OFFSET_FRAME_0_LENGTH + i] = (uint8_t)frames[i].length;
        memcpy(&packet[payload_offset], frames[i].data, frames[i].length);
        payload_offset += frames[i].length;
    }

    memcpy(out_packet, packet, packet_length);
    *out_length = packet_length;
    return FHSS_AUDIO_PACKET_STATUS_OK;
}

fhss_audio_packet_status_t fhss_audio_packet_unpack(
    const uint8_t *packet,
    size_t packet_length,
    fhss_audio_packet_view_t *out_view
)
{
    if (packet == NULL || out_view == NULL) {
        return FHSS_AUDIO_PACKET_STATUS_INVALID_ARGUMENT;
    }
    if (packet_length < FHSS_AUDIO_PACKET_HEADER_SIZE ||
        packet_length > RF_TRANSPORT_MAX_PACKET_LENGTH ||
        packet[OFFSET_MAGIC] != FHSS_AUDIO_PACKET_MAGIC ||
        packet[OFFSET_VERSION] != FHSS_AUDIO_PACKET_VERSION ||
        packet[OFFSET_TYPE] != FHSS_AUDIO_PACKET_TYPE ||
        !flags_are_valid(packet[OFFSET_FLAGS])) {
        return FHSS_AUDIO_PACKET_STATUS_INVALID_FORMAT;
    }

    const uint8_t frame_count = packet[OFFSET_FRAME_COUNT];
    if (frame_count == 0U || frame_count > FHSS_AUDIO_PACKET_MAX_FRAMES) {
        return FHSS_AUDIO_PACKET_STATUS_INVALID_FORMAT;
    }
    if (frame_count == 1U && packet[OFFSET_FRAME_1_LENGTH] != 0U) {
        return FHSS_AUDIO_PACKET_STATUS_INVALID_FORMAT;
    }

    size_t payload_length = 0U;
    for (size_t i = 0U; i < frame_count; ++i) {
        const size_t frame_length = packet[OFFSET_FRAME_0_LENGTH + i];
        if (frame_length == 0U || frame_length >
            FHSS_AUDIO_PACKET_MAX_PAYLOAD_BYTES - payload_length) {
            return FHSS_AUDIO_PACKET_STATUS_INVALID_FORMAT;
        }
        payload_length += frame_length;
    }
    if (packet_length != FHSS_AUDIO_PACKET_HEADER_SIZE + payload_length) {
        return FHSS_AUDIO_PACKET_STATUS_INVALID_FORMAT;
    }

    fhss_audio_packet_view_t view = {
        .sequence = (uint16_t)packet[OFFSET_SEQUENCE_LO] |
                    ((uint16_t)packet[OFFSET_SEQUENCE_HI] << 8U),
        .flags = packet[OFFSET_FLAGS],
        .frame_count = frame_count,
    };

    size_t payload_offset = FHSS_AUDIO_PACKET_HEADER_SIZE;
    for (size_t i = 0U; i < frame_count; ++i) {
        const size_t frame_length = packet[OFFSET_FRAME_0_LENGTH + i];
        view.frames[i].data = &packet[payload_offset];
        view.frames[i].length = frame_length;
        payload_offset += frame_length;
    }

    *out_view = view;
    return FHSS_AUDIO_PACKET_STATUS_OK;
}

fhss_audio_packet_status_t fhss_audio_end_packet_pack(
    const fhss_audio_end_packet_t *end,
    uint8_t *out_packet,
    size_t out_capacity,
    size_t *out_length
)
{
    if (end == NULL || out_packet == NULL || out_length == NULL) {
        return FHSS_AUDIO_PACKET_STATUS_INVALID_ARGUMENT;
    }
    if (out_capacity < FHSS_AUDIO_END_PACKET_SIZE) {
        return FHSS_AUDIO_PACKET_STATUS_BUFFER_TOO_SMALL;
    }

    const uint8_t packet[FHSS_AUDIO_END_PACKET_SIZE] = {
        FHSS_AUDIO_PACKET_MAGIC,
        FHSS_AUDIO_PACKET_VERSION,
        FHSS_AUDIO_END_PACKET_TYPE,
        (uint8_t)end->reason,
        (uint8_t)(end->session_id & 0xFFU),
        (uint8_t)(end->session_id >> 8U),
        (uint8_t)(end->final_sequence & 0xFFU),
        (uint8_t)(end->final_sequence >> 8U),
        0U,
    };
    memcpy(out_packet, packet, sizeof(packet));
    *out_length = sizeof(packet);
    return FHSS_AUDIO_PACKET_STATUS_OK;
}

fhss_audio_packet_status_t fhss_audio_end_packet_unpack(
    const uint8_t *packet,
    size_t packet_length,
    fhss_audio_end_packet_t *out_end
)
{
    if (packet == NULL || out_end == NULL) {
        return FHSS_AUDIO_PACKET_STATUS_INVALID_ARGUMENT;
    }
    if (packet_length != FHSS_AUDIO_END_PACKET_SIZE ||
        packet[0] != FHSS_AUDIO_PACKET_MAGIC ||
        packet[1] != FHSS_AUDIO_PACKET_VERSION ||
        packet[2] != FHSS_AUDIO_END_PACKET_TYPE ||
        packet[3] != FHSS_AUDIO_END_REASON_PTT_RELEASE ||
        packet[8] != 0U) {
        return FHSS_AUDIO_PACKET_STATUS_INVALID_FORMAT;
    }

    out_end->reason = (fhss_audio_end_reason_t)packet[3];
    out_end->session_id = (uint16_t)packet[4] | ((uint16_t)packet[5] << 8U);
    out_end->final_sequence =
        (uint16_t)packet[6] | ((uint16_t)packet[7] << 8U);
    return FHSS_AUDIO_PACKET_STATUS_OK;
}
