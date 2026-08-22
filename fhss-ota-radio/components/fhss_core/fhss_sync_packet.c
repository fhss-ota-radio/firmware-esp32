#include "fhss_sync_packet.h"

bool fhss_sync_packet_has_valid_magic(
    const uint8_t *buffer,
    size_t buffer_length
)
{
    return buffer != NULL &&
           buffer_length == OTA_FHSS_SYNC_PACKET_SIZE &&
           buffer[0] == (uint8_t)OTA_PKT_FHSS_SYNC &&
           buffer[1] == OTA_FHSS_SYNC_VERSION;
}

fhss_packet_status_t fhss_sync_packet_encode(
    const fhss_sync_packet_t *packet,
    uint8_t *buffer,
    size_t buffer_capacity,
    size_t *out_length
)
{
    if (out_length != NULL) {
        *out_length = 0U;
    }
    if (packet == NULL || buffer == NULL || out_length == NULL) {
        return FHSS_PACKET_STATUS_INVALID_ARG;
    }
    if (buffer_capacity < OTA_FHSS_SYNC_PACKET_SIZE) {
        return FHSS_PACKET_STATUS_BUFFER_TOO_SMALL;
    }
    if (packet->version != OTA_FHSS_SYNC_VERSION) {
        return FHSS_PACKET_STATUS_UNSUPPORTED_VERSION;
    }

    const ota_fhss_sync_fields_t fields = {
        .sync_version = packet->version,
        .generation = packet->generation,
        .sequence = packet->sequence,
        .hop_index = packet->hop_index,
        .slot_number = packet->slot_number,
    };
    *out_length = ota_protocol_encode_fhss_sync(
        buffer, buffer_capacity, &fields);
    return *out_length == OTA_FHSS_SYNC_PACKET_SIZE
        ? FHSS_PACKET_STATUS_OK
        : FHSS_PACKET_STATUS_INVALID_TYPE;
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
    if (buffer_length != OTA_FHSS_SYNC_PACKET_SIZE) {
        return FHSS_PACKET_STATUS_INVALID_LENGTH;
    }
    if (buffer[0] != (uint8_t)OTA_PKT_FHSS_SYNC) {
        return FHSS_PACKET_STATUS_INVALID_MAGIC;
    }
    if (buffer[1] != OTA_FHSS_SYNC_VERSION) {
        return FHSS_PACKET_STATUS_UNSUPPORTED_VERSION;
    }

    ota_fhss_sync_fields_t fields = {0};
    if (!ota_protocol_decode_fhss_sync(buffer, buffer_length, &fields)) {
        return FHSS_PACKET_STATUS_INVALID_TYPE;
    }
    const fhss_sync_packet_t decoded = {
        .version = fields.sync_version,
        .generation = fields.generation,
        .sequence = fields.sequence,
        .hop_index = fields.hop_index,
        .slot_number = fields.slot_number,
    };
    *out_packet = decoded;
    return FHSS_PACKET_STATUS_OK;
}
