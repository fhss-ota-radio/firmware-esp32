#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FHSS_SYNC_PACKET_MAGIC       0x46485353UL
#define FHSS_SYNC_PACKET_VERSION     2U
#define FHSS_SYNC_PACKET_LENGTH      17U

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

typedef struct {
    uint8_t version;
    fhss_packet_type_t type;
    uint16_t sequence;
    uint8_t hop_index;
    uint32_t slot_number;
    /* TX가 세션마다 새로 생성해 평문으로 실어 보내는 값. secret_seed(양쪽에
     * 미리 공유된 비밀 키)와 HMAC-SHA256으로 조합해 그 세션만의 hop_seed를
     * 만드는 데 쓴다 — 이 값 자체는 공개돼도 안전하다. */
    uint32_t public_seed;
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