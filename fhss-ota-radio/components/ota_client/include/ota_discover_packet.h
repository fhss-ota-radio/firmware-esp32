#pragma once

#include <stddef.h>
#include <stdint.h>

#include "device_id_config.h" /* DEVICE_ID_LEN */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * OTA_DISCOVER 패킷 (Qt 앱 -> ESP, 2바이트): Qt 앱이 OTA 대기 중인 기기를
 * 찾으려고 방송하는 스캔 신호. version/type 각 1바이트 — 정확한 값은 아직
 * 미확정(TODO, 팀2/Qt 쪽과 합의 필요), 길이(2바이트)와 필드 구성만 먼저
 * 확정해서 수신 구조를 미리 짜둔다.
 */
#define OTA_DISCOVER_PACKET_LENGTH 2U

typedef struct {
    uint8_t version; /* 이 패킷을 어떤 규격으로 해석할지 (패킷 프로토콜 버전) */
    uint8_t type;    /* "이게 DISCOVER 패킷이다"라는 표시 자체 */
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
 * OTA_DISCOVER_ACK 패킷 (ESP -> Qt 앱, DEVICE_ID_LEN+3 = 6바이트): DISCOVER를
 * 수신했고 자신이 MENU_OTA 상태일 때 회신. device_id(DEVICE_ID_LEN바이트,
 * components/device_id)와 firmware_version(3바이트, major/minor/patch —
 * main/firmware_version.h) 순서로 이어붙인다. 이 firmware_version은
 * OTA_DISCOVER 패킷의 version 필드(패킷 규격 버전)와는 다른 값이니 혼동 금지.
 */
#define OTA_DISCOVER_ACK_LENGTH (DEVICE_ID_LEN + 3U)

typedef struct {
    uint8_t device_id[DEVICE_ID_LEN];
    uint8_t firmware_version[3]; /* [0]=major, [1]=minor, [2]=patch */
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
