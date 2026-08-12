#include "ota_discover_packet.h"

ota_discover_status_t ota_discover_packet_decode(
    const uint8_t *buffer,
    size_t buffer_length,
    ota_discover_packet_t *out_packet
)
{
    if (buffer == NULL || out_packet == NULL) {
        return OTA_DISCOVER_STATUS_INVALID_ARG;
    }
    if (buffer_length != OTA_DISCOVER_PACKET_LENGTH) {
        return OTA_DISCOVER_STATUS_INVALID_LENGTH;
    }

    out_packet->version = buffer[0];
    out_packet->type = buffer[1];
    return OTA_DISCOVER_STATUS_OK;
}

ota_discover_status_t ota_discover_ack_encode(
    const ota_discover_ack_t *ack,
    uint8_t *buffer,
    size_t buffer_capacity,
    size_t *out_length
)
{
    if (ack == NULL || buffer == NULL || out_length == NULL) {
        return OTA_DISCOVER_STATUS_INVALID_ARG;
    }
    if (buffer_capacity < OTA_DISCOVER_ACK_LENGTH) {
        return OTA_DISCOVER_STATUS_INVALID_LENGTH;
    }

    size_t offset = 0;
    for (size_t i = 0; i < DEVICE_ID_LEN; i++) {
        buffer[offset++] = ack->device_id[i];
    }
    for (size_t i = 0; i < 3; i++) {
        buffer[offset++] = ack->firmware_version[i];
    }

    *out_length = offset;
    return OTA_DISCOVER_STATUS_OK;
}
