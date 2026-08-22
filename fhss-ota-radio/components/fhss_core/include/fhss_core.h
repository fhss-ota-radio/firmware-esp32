#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fhss_hop_sequence.h"
#include "fhss_sync_packet.h"
#include "fhss_sync_state.h"
#include "fhss_timing_window.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FHSS_CORE_STATUS_OK = 0,
    FHSS_CORE_STATUS_INVALID_ARG,
    FHSS_CORE_STATUS_INVALID_CONFIG,
    FHSS_CORE_STATUS_NOT_INITIALIZED,
    FHSS_CORE_STATUS_PACKET_ERROR,
    FHSS_CORE_STATUS_TIMING_ERROR,
    FHSS_CORE_STATUS_SYNC_ERROR,
    FHSS_CORE_STATUS_HOP_ERROR,
} fhss_core_status_t;


/*
 * fhss_core 전체 설정
 *
 * channels / channel_count
 *     → Hop Sequence 초기화에 사용
 *
 * timing
 *     → 수신 Timing Window 설정
 *
 * sync
 *     → 동기 획득/상실 threshold 설정
 */
typedef struct {
    const uint8_t *channels;
    size_t channel_count;
    uint32_t hop_seed;
    uint32_t generation;
    uint8_t reserved_channel;

    fhss_timing_window_config_t timing;
    fhss_sync_state_config_t sync;
} fhss_core_config_t;


/*
 * fhss_core 내부 상태
 *
 * 외부 모듈에서는 가능하면 직접 수정하지 않고
 * fhss_core API를 통해 접근한다.
 */
typedef struct {
    fhss_hop_sequence_t hop_sequence;
    fhss_sync_state_tracker_t sync_tracker;
    fhss_timing_window_config_t timing_config;
    uint32_t generation;

    bool initialized;
} fhss_core_t;


/*
 * SYNC 패킷 하나를 처리한 결과
 */
typedef struct {
    fhss_sync_packet_t packet;

    fhss_timing_window_evaluation_t timing;

    fhss_sync_event_t sync_event;
    fhss_sync_state_t sync_state;

    uint8_t hop_index;
    uint8_t channel;
} fhss_core_rx_result_t;


/*
 * fhss_core 초기화
 */
fhss_core_status_t fhss_core_init(
    fhss_core_t *core,
    const fhss_core_config_t *config
);


/*
 * 수신한 SYNC 패킷 처리
 *
 * 입력:
 *   raw packet
 *   packet length
 *   예상 수신 시각
 *   실제 수신 시각
 *
 * 출력:
 *   decode 결과
 *   timing 결과
 *   sync 상태 / 이벤트
 *   hop index / channel
 */
fhss_core_status_t fhss_core_process_rx(
    fhss_core_t *core,
    const uint8_t *buffer,
    size_t buffer_length,
    int64_t expected_rx_time_us,
    int64_t actual_rx_time_us,
    fhss_core_rx_result_t *out_result
);


/*
 * 예상 시간에 SYNC 패킷을 받지 못했을 때 호출
 */
fhss_core_status_t fhss_core_handle_timeout(
    fhss_core_t *core,
    fhss_sync_event_t *out_event,
    fhss_sync_state_t *out_state
);


/*
 * 현재 slot에 대응되는 channel 조회
 */
fhss_core_status_t fhss_core_get_channel(
    const fhss_core_t *core,
    uint32_t slot_number,
    uint8_t *out_channel
);


#ifdef __cplusplus
}
#endif
