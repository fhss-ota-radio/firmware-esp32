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

    if (buffer[0] != OTA_DISCOVER_PACKET_TYPE) {
        return OTA_DISCOVER_STATUS_INVALID_ARG;
    }

    out_packet->type = buffer[0];
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
    if (ack->device_id > OTA_DEVICE_ID_MAX) {
        return OTA_DISCOVER_STATUS_INVALID_ARG;
    }

    size_t offset = 0;
    buffer[offset++] = OTA_DISCOVER_ACK_PACKET_TYPE;
    buffer[offset++] = (uint8_t)(ack->device_id & 0xFFU);
    buffer[offset++] = (uint8_t)((ack->device_id >> 8) & 0xFFU);
    buffer[offset++] = (uint8_t)((ack->device_id >> 16) & 0xFFU);
    buffer[offset++] = ack->fw_major;
    buffer[offset++] = ack->fw_minor;
    buffer[offset++] = ack->fw_patch;

    *out_length = offset;
    return OTA_DISCOVER_STATUS_OK;
}
