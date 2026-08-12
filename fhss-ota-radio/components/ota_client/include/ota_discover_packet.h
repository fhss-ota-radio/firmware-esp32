#pragma once

#include <stddef.h>
#include <stdint.h>

#include "device_id_config.h" /* DEVICE_ID_LEN */

#ifdef __cplusplus
extern "C" {
#endif

/* ota-protocol v0.2 (9d2aa2b): 공통 version 없이 type만 전송한다. */
#define OTA_DISCOVER_PACKET_TYPE 6U
#define OTA_DISCOVER_ACK_PACKET_TYPE 7U
#define OTA_DISCOVER_PACKET_LENGTH 1U
#define OTA_DEVICE_ID_MAX 0xFFFFFFU

typedef struct {
    uint8_t type;
} ota_discover_packet_t;

typedef enum {
    OTA_DISCOVER_STATUS_OK = 0,
    OTA_DISCOVER_STATUS_INVALID_ARG,
    OTA_DISCOVER_STATUS_INVALID_LENGTH,
} ota_discover_status_t;

/* buffer_length가 OTA_DISCOVER_PACKET_LENGTH가 아니면 실패. */
ota_discover_status_t ota_discover_packet_decode(
    const uint8_t *buffer,
    size_t buffer_length,
    ota_discover_packet_t *out_packet
);

/*
 * OTA_DISCOVER_ACK 패킷 (ESP -> Qt 앱, 7바이트): type(1), device_id(3,
 * Little Endian), firmware version major/minor/patch(3) 순서다.
 */
#define OTA_DISCOVER_ACK_LENGTH (1U + DEVICE_ID_LEN + 3U)

typedef struct {
    uint32_t device_id;
    uint8_t fw_major;
    uint8_t fw_minor;
    uint8_t fw_patch;
} ota_discover_ack_t;

ota_discover_status_t ota_discover_ack_encode(
    const ota_discover_ack_t *ack,
    uint8_t *buffer,
    size_t buffer_capacity,
    size_t *out_length
);

#ifdef __cplusplus
}
#endif
