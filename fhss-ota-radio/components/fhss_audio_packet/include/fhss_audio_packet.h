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
#define FHSS_AUDIO_END_PACKET_TYPE          0x03U
#define FHSS_AUDIO_END_PACKET_SIZE          9U
#define FHSS_AUDIO_END_NO_AUDIO_SEQUENCE    UINT16_MAX
/* HMAC 홉 시드 파생용 public_seed 통지 패킷. FHSS SYNC(ota_protocol 공유
 * 서브모듈 포맷, generation만 실림)와는 별개 — 서브모듈에 필드를 추가하는
 * PR 없이 세션별 public_seed를 실어 보내기 위해 이 계층(우리 쪽 소유)에
 * 새 타입으로 정의한다. MAGIC(0xA5)이 ota_protocol의 타입 바이트(1~10)와
 * 절대 안 겹치므로 fhss_sync_packet_has_valid_magic()과 안전하게 구분됨. */
#define FHSS_AUDIO_SEED_ANNOUNCE_PACKET_TYPE 0x04U
#define FHSS_AUDIO_SEED_ANNOUNCE_PACKET_SIZE 9U
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

typedef enum {
    FHSS_AUDIO_END_REASON_PTT_RELEASE = 0,
} fhss_audio_end_reason_t;

typedef struct {
    uint16_t session_id;
    uint16_t final_sequence;
    fhss_audio_end_reason_t reason;
} fhss_audio_end_packet_t;

typedef struct {
    uint16_t session_id;
    uint32_t public_seed;
} fhss_audio_seed_announce_packet_t;

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

fhss_audio_packet_status_t fhss_audio_end_packet_pack(
    const fhss_audio_end_packet_t *end,
    uint8_t *out_packet,
    size_t out_capacity,
    size_t *out_length
);

fhss_audio_packet_status_t fhss_audio_end_packet_unpack(
    const uint8_t *packet,
    size_t packet_length,
    fhss_audio_end_packet_t *out_end
);

fhss_audio_packet_status_t fhss_audio_seed_announce_packet_pack(
    const fhss_audio_seed_announce_packet_t *announce,
    uint8_t *out_packet,
    size_t out_capacity,
    size_t *out_length
);

fhss_audio_packet_status_t fhss_audio_seed_announce_packet_unpack(
    const uint8_t *packet,
    size_t packet_length,
    fhss_audio_seed_announce_packet_t *out_announce
);

#ifdef __cplusplus
}
#endif
