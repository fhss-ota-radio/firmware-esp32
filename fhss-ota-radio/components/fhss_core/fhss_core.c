#include "fhss_core.h"

#include <stddef.h>


static fhss_core_status_t validate_core(
    const fhss_core_t *core
)
{
    if (core == NULL) {
        return FHSS_CORE_STATUS_INVALID_ARG;
    }

    if (!core->initialized) {
        return FHSS_CORE_STATUS_NOT_INITIALIZED;
    }

    return FHSS_CORE_STATUS_OK;
}


fhss_core_status_t fhss_core_init(
    fhss_core_t *core,
    const fhss_core_config_t *config
)
{
    /* 1. 입력 포인터 검사 */
    if (core == NULL || config == NULL) {
        return FHSS_CORE_STATUS_INVALID_ARG;
    }

    if (config->channels == NULL ||
        config->channel_count == 0U) {
        return FHSS_CORE_STATUS_INVALID_CONFIG;
    }

    /*
     * 초기화 중 오류가 발생했을 때
     * 호출자의 core를 부분적으로 수정하지 않기 위해
     * 임시 구조체를 사용한다.
     */
    fhss_core_t initialized_core = {
        .timing_config = config->timing,
        .initialized = false,
    };


    /* 2. Hop Sequence 초기화 */
    const fhss_hop_status_t hop_status =
        fhss_hop_sequence_init_seeded(
            &initialized_core.hop_sequence,
            config->channels,
            config->channel_count,
            config->hop_seed,
            config->reserved_channel
        );

    if (hop_status != FHSS_HOP_STATUS_OK) {
        return FHSS_CORE_STATUS_HOP_ERROR;
    }


    /* 3. Sync State 초기화 */
    const fhss_sync_status_t sync_status =
        fhss_sync_state_init(
            &initialized_core.sync_tracker,
            &config->sync
        );

    if (sync_status != FHSS_SYNC_STATUS_OK) {
        return FHSS_CORE_STATUS_SYNC_ERROR;
    }


    /* 4. 초기화 완료 */
    initialized_core.initialized = true;


    /* 5. 호출자의 core에 최종 결과 복사 */
    *core = initialized_core;

    return FHSS_CORE_STATUS_OK;
}


fhss_core_status_t fhss_core_process_rx(
    fhss_core_t *core,
    const uint8_t *buffer,
    size_t buffer_length,
    int64_t expected_rx_time_us,
    int64_t actual_rx_time_us,
    fhss_core_rx_result_t *out_result
)
{
    /* 1. 출력 및 입력 포인터 검사 */
    if (buffer == NULL || out_result == NULL) {
        return FHSS_CORE_STATUS_INVALID_ARG;
    }


    /* 2. core 초기화 상태 검사 */
    const fhss_core_status_t core_status =
        validate_core(core);

    if (core_status != FHSS_CORE_STATUS_OK) {
        return core_status;
    }


    /*
     * 처리 중간에 오류가 발생하더라도
     * out_result에 부분 결과를 남기지 않도록
     * 지역 구조체를 사용한다.
     */
    fhss_core_rx_result_t result = {0};


    /* 3. SYNC 패킷 decode */
    const fhss_packet_status_t packet_status =
        fhss_sync_packet_decode(
            buffer,
            buffer_length,
            &result.packet
        );

    if (packet_status != FHSS_PACKET_STATUS_OK) {
        return FHSS_CORE_STATUS_PACKET_ERROR;
    }


    /* 4. 수신 Timing Window 판정 */
    const fhss_timing_status_t timing_status =
        fhss_timing_window_evaluate(
            &core->timing_config,
            expected_rx_time_us,
            actual_rx_time_us,
            &result.timing
        );

    if (timing_status != FHSS_TIMING_STATUS_OK) {
        return FHSS_CORE_STATUS_TIMING_ERROR;
    }


    /*
     * 5. Timing Window 결과를 Sync State에 반영
     *
     * INSIDE_WINDOW
     *     → 정상 수신(VALID)
     *
     * BEFORE / AFTER
     *     → 유효한 동기 수신으로 인정하지 않음(MISS)
     */
    fhss_sync_status_t sync_status;

    if (result.timing.result == FHSS_TIMING_INSIDE_WINDOW) {

        sync_status =
            fhss_sync_state_on_valid(
                &core->sync_tracker,
                &result.sync_event
            );

    } else {

        sync_status =
            fhss_sync_state_on_miss(
                &core->sync_tracker,
                &result.sync_event
            );
    }

    if (sync_status != FHSS_SYNC_STATUS_OK) {
        return FHSS_CORE_STATUS_SYNC_ERROR;
    }


    /* 6. 현재 Sync State 조회 */
    sync_status =
        fhss_sync_state_get(
            &core->sync_tracker,
            &result.sync_state
        );

    if (sync_status != FHSS_SYNC_STATUS_OK) {
        return FHSS_CORE_STATUS_SYNC_ERROR;
    }


    /*
     * 7. packet의 slot_number를 기준으로
     *    현재 Hop Index 계산
     */
    const fhss_hop_status_t hop_index_status =
        fhss_hop_sequence_get_index(
            &core->hop_sequence,
            result.packet.slot_number,
            &result.hop_index
        );

    if (hop_index_status != FHSS_HOP_STATUS_OK) {
        return FHSS_CORE_STATUS_HOP_ERROR;
    }


    /*
     * 8. slot_number에 대응되는 실제 channel 계산
     */
    const fhss_hop_status_t channel_status =
        fhss_hop_sequence_get_channel(
            &core->hop_sequence,
            result.packet.slot_number,
            &result.channel
        );

    if (channel_status != FHSS_HOP_STATUS_OK) {
        return FHSS_CORE_STATUS_HOP_ERROR;
    }


    /* 9. 모든 처리 성공 후 결과 복사 */
    *out_result = result;

    return FHSS_CORE_STATUS_OK;
}


fhss_core_status_t fhss_core_handle_timeout(
    fhss_core_t *core,
    fhss_sync_event_t *out_event,
    fhss_sync_state_t *out_state
)
{
    /* 1. 출력 포인터 검사 */
    if (out_event == NULL || out_state == NULL) {
        return FHSS_CORE_STATUS_INVALID_ARG;
    }


    /* 2. core 상태 검사 */
    const fhss_core_status_t core_status =
        validate_core(core);

    if (core_status != FHSS_CORE_STATUS_OK) {
        return core_status;
    }


    /* 3. 패킷 미수신을 MISS로 처리 */
    fhss_sync_event_t event = FHSS_SYNC_EVENT_NONE;

    fhss_sync_status_t sync_status =
        fhss_sync_state_on_miss(
            &core->sync_tracker,
            &event
        );

    if (sync_status != FHSS_SYNC_STATUS_OK) {
        return FHSS_CORE_STATUS_SYNC_ERROR;
    }


    /* 4. MISS 처리 후 현재 상태 조회 */
    fhss_sync_state_t state;

    sync_status =
        fhss_sync_state_get(
            &core->sync_tracker,
            &state
        );

    if (sync_status != FHSS_SYNC_STATUS_OK) {
        return FHSS_CORE_STATUS_SYNC_ERROR;
    }


    /* 5. 결과 반환 */
    *out_event = event;
    *out_state = state;

    return FHSS_CORE_STATUS_OK;
}


fhss_core_status_t fhss_core_get_channel(
    const fhss_core_t *core,
    uint32_t slot_number,
    uint8_t *out_channel
)
{
    /* 1. 출력 포인터 검사 */
    if (out_channel == NULL) {
        return FHSS_CORE_STATUS_INVALID_ARG;
    }


    /* 2. core 상태 검사 */
    const fhss_core_status_t core_status =
        validate_core(core);

    if (core_status != FHSS_CORE_STATUS_OK) {
        return core_status;
    }


    /* 3. slot → channel 계산 */
    const fhss_hop_status_t hop_status =
        fhss_hop_sequence_get_channel(
            &core->hop_sequence,
            slot_number,
            out_channel
        );

    if (hop_status != FHSS_HOP_STATUS_OK) {
        return FHSS_CORE_STATUS_HOP_ERROR;
    }


    return FHSS_CORE_STATUS_OK;
}
