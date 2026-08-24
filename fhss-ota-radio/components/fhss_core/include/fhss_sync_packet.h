#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ota_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FHSS_SYNC_PACKET_VERSION     OTA_FHSS_SYNC_VERSION
#define FHSS_SYNC_PACKET_LENGTH      OTA_FHSS_SYNC_PACKET_SIZE

typedef enum {
    FHSS_PACKET_TYPE_SYNC = 1,
} fhss_packet_type_t;

typedef enum {
    FHSS_PACKET_STATUS_OK = 0,
    FHSS_PACKET_STATUS_INVALID_ARG,
    FHSS_PACKET_STATUS_INVALID_LENGTH,
    FHSS_PACKET_STATUS_INVALID_MAGIC,
    FHSS_PACKET_STATUS_UNSUPPORTED_VERSION,
    FHSS_PACKET_STATUS_INVALID_TYPE,
    FHSS_PACKET_STATUS_BUFFER_TOO_SMALL,
} fhss_packet_status_t;

/* public_seed(HMAC 파생용)는 이 구조체에 없다 — 이 패킷은 ota_protocol(팀
 * 공유 서브모듈)의 ota_fhss_sync_fields_t를 그대로 위임해서 인코딩/디코딩
 * 하므로, 서브모듈에 필드를 추가하는 PR 없이는 여기 넣을 수 없다.
 * public_seed는 대신 fhss_audio_packet의 별도 announce 패킷(서브모듈 밖,
 * 우리 쪽에서만 정의)으로 전달한다 — fhss_audio_adapter.c 참고. */
typedef struct {
    uint8_t version;
    uint32_t generation;
    uint16_t sequence;
    uint8_t hop_index;
    uint32_t slot_number;
} fhss_sync_packet_t;

/**
 * @brief 동기화 패킷을 바이트 배열로 변환한다.
 */
fhss_packet_status_t fhss_sync_packet_encode(
    const fhss_sync_packet_t *packet,
    uint8_t *buffer,
    size_t buffer_capacity,
    size_t *out_length
);

/**
 * @brief 바이트 배열을 검증하고 동기화 패킷으로 변환한다.
 */
fhss_packet_status_t fhss_sync_packet_decode(
    const uint8_t *buffer,
    size_t buffer_length,
    fhss_sync_packet_t *out_packet
);

/**
 * @brief 바이트 배열의 매직넘버가 올바른지 확인한다.
 */
bool fhss_sync_packet_has_valid_magic(
    const uint8_t *buffer,
    size_t buffer_length
);

#ifdef __cplusplus
}
#endif
