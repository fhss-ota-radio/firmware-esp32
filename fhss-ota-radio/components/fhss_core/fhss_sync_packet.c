#include "fhss_sync_packet.h"

/*
 * 무선 패킷의 다중 바이트 정수는 big-endian으로 저장한다.
 */

static void write_u16_be(uint8_t *buffer, uint16_t value)
{
    buffer[0] = (uint8_t)((value >> 8) & 0xFFU);
    buffer[1] = (uint8_t)(value & 0xFFU);
}

static void write_u32_be(uint8_t *buffer, uint32_t value)
{
    buffer[0] = (uint8_t)((value >> 24) & 0xFFU);
    buffer[1] = (uint8_t)((value >> 16) & 0xFFU);
    buffer[2] = (uint8_t)((value >> 8) & 0xFFU);
    buffer[3] = (uint8_t)(value & 0xFFU);
}

static uint16_t read_u16_be(const uint8_t *buffer)
{
    return (uint16_t)(
        ((uint16_t)buffer[0] << 8) |
        (uint16_t)buffer[1]
    );
}

static uint32_t read_u32_be(const uint8_t *buffer)
{
    return ((uint32_t)buffer[0] << 24) |
           ((uint32_t)buffer[1] << 16) |
           ((uint32_t)buffer[2] << 8) |
           (uint32_t)buffer[3];
}

static bool is_valid_packet_type(fhss_packet_type_t type)
{
    return type == FHSS_PACKET_TYPE_SYNC;
}

bool fhss_sync_packet_has_valid_magic(
    const uint8_t *buffer,
    size_t buffer_length
)
{
    if (buffer == NULL || buffer_length < sizeof(uint32_t)) {
        return false;
    }

    return read_u32_be(buffer) == (uint32_t)FHSS_SYNC_PACKET_MAGIC;
}

fhss_packet_status_t fhss_sync_packet_encode(
    const fhss_sync_packet_t *packet,
    uint8_t *buffer,
    size_t buffer_capacity,
    size_t *out_length
)
{
    /*
     * 실패했을 때 이전 길이값이 남지 않도록 먼저 0으로 초기화한다.
     */
    if (out_length != NULL) {
        *out_length = 0U;
    }

    if (packet == NULL || buffer == NULL || out_length == NULL) {
        return FHSS_PACKET_STATUS_INVALID_ARG;
    }

    if (buffer_capacity < FHSS_SYNC_PACKET_LENGTH) {
        return FHSS_PACKET_STATUS_BUFFER_TOO_SMALL;
    }

    if (packet->version != FHSS_SYNC_PACKET_VERSION) {
        return FHSS_PACKET_STATUS_UNSUPPORTED_VERSION;
    }

    if (!is_valid_packet_type(packet->type)) {
        return FHSS_PACKET_STATUS_INVALID_TYPE;
    }

    /*
     * Offset  Size  Field
     * 0       4     Magic
     * 4       1     Version
     * 5       1     Packet Type
     * 6       2     Sequence
     * 8       1     Hop Index
     * 9       4     Slot Number
     */
    write_u32_be(&buffer[0], (uint32_t)FHSS_SYNC_PACKET_MAGIC);

    buffer[4] = packet->version;
    buffer[5] = (uint8_t)packet->type;

    write_u16_be(&buffer[6], packet->sequence);

    buffer[8] = packet->hop_index;

    write_u32_be(&buffer[9], packet->slot_number);

    *out_length = FHSS_SYNC_PACKET_LENGTH;

    return FHSS_PACKET_STATUS_OK;
}

fhss_packet_status_t fhss_sync_packet_decode(
    const uint8_t *buffer,
    size_t buffer_length,
    fhss_sync_packet_t *out_packet
)
{
    if (buffer == NULL || out_packet == NULL) {
        return FHSS_PACKET_STATUS_INVALID_ARG;
    }

    if (buffer_length != FHSS_SYNC_PACKET_LENGTH) {
        return FHSS_PACKET_STATUS_INVALID_LENGTH;
    }

    if (!fhss_sync_packet_has_valid_magic(buffer, buffer_length)) {
        return FHSS_PACKET_STATUS_INVALID_MAGIC;
    }

    const uint8_t version = buffer[4];

    if (version != FHSS_SYNC_PACKET_VERSION) {
        return FHSS_PACKET_STATUS_UNSUPPORTED_VERSION;
    }

    const fhss_packet_type_t type =
        (fhss_packet_type_t)buffer[5];

    if (!is_valid_packet_type(type)) {
        return FHSS_PACKET_STATUS_INVALID_TYPE;
    }

    /*
     * 검증이 모두 끝난 뒤 임시 구조체를 완성한다.
     * 오류가 발생했을 때 out_packet이 일부만 변경되는 것을 막는다.
     */
    const fhss_sync_packet_t decoded_packet = {
        .version = version,
        .type = type,
        .sequence = read_u16_be(&buffer[6]),
        .hop_index = buffer[8],
        .slot_number = read_u32_be(&buffer[9]),
    };

    *out_packet = decoded_packet;

    return FHSS_PACKET_STATUS_OK;
}