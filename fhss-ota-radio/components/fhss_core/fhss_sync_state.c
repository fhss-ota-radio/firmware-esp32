#include "fhss_sync_state.h"

#include <stddef.h>


static fhss_sync_status_t validate_tracker(
    const fhss_sync_state_tracker_t *tracker
)
{
    if (tracker == NULL) {
        return FHSS_SYNC_STATUS_INVALID_ARG;
    }

    if (tracker->initialized == 0U) {
        return FHSS_SYNC_STATUS_NOT_INITIALIZED;
    }

    return FHSS_SYNC_STATUS_OK;
}


fhss_sync_status_t fhss_sync_state_init(
    fhss_sync_state_tracker_t *tracker,
    const fhss_sync_state_config_t *config
)
{
    /* 1. 입력 포인터 검사 */
    if (tracker == NULL || config == NULL) {
        return FHSS_SYNC_STATUS_INVALID_ARG;
    }

    /* 2. 설정값 검사 */
    if (config->acquire_count == 0U ||
        config->loss_count == 0U) {
        return FHSS_SYNC_STATUS_INVALID_CONFIG;
    }

    /* 3. 초기 상태 생성 */
    fhss_sync_state_tracker_t initialized_tracker = {
        .state = FHSS_SYNC_STATE_SEARCHING,
        .consecutive_valid = 0U,
        .consecutive_misses = 0U,
        .config = *config,
        .initialized = 1U,
    };

    /* 4. 호출자가 전달한 tracker에 복사 */
    *tracker = initialized_tracker;

    return FHSS_SYNC_STATUS_OK;
}


fhss_sync_status_t fhss_sync_state_on_valid(
    fhss_sync_state_tracker_t *tracker,
    fhss_sync_event_t *out_event
)
{
    /* 1. 출력 포인터 검사 */
    if (out_event == NULL) {
        return FHSS_SYNC_STATUS_INVALID_ARG;
    }

    /* 2. tracker 상태 검사 */
    const fhss_sync_status_t status = validate_tracker(tracker);

    if (status != FHSS_SYNC_STATUS_OK) {
        return status;
    }

    /* 3. 기본 이벤트는 NONE */
    *out_event = FHSS_SYNC_EVENT_NONE;

    /* 4. 정상 수신했으므로 MISS 카운터 초기화 */
    tracker->consecutive_misses = 0U;

    /* 5. SEARCHING 상태일 때만 정상 수신 횟수 누적 */
    if (tracker->state == FHSS_SYNC_STATE_SEARCHING) {

        tracker->consecutive_valid++;

        /* 6. 기준 횟수에 도달하면 동기 획득 */
        if (tracker->consecutive_valid >=
            tracker->config.acquire_count) {

            tracker->state = FHSS_SYNC_STATE_LOCKED;
            tracker->consecutive_valid = 0U;

            *out_event = FHSS_SYNC_EVENT_ACQUIRED;
        }
    }

    return FHSS_SYNC_STATUS_OK;
}


fhss_sync_status_t fhss_sync_state_on_miss(
    fhss_sync_state_tracker_t *tracker,
    fhss_sync_event_t *out_event
)
{
    /* 1. 출력 포인터 검사 */
    if (out_event == NULL) {
        return FHSS_SYNC_STATUS_INVALID_ARG;
    }

    /* 2. tracker 상태 검사 */
    const fhss_sync_status_t status = validate_tracker(tracker);

    if (status != FHSS_SYNC_STATUS_OK) {
        return status;
    }

    /* 3. 기본 이벤트는 NONE */
    *out_event = FHSS_SYNC_EVENT_NONE;

    /* 4. MISS 발생 시 VALID 연속 횟수 초기화 */
    tracker->consecutive_valid = 0U;

    /* 5. LOCKED 상태일 때만 MISS 횟수 누적 */
    if (tracker->state == FHSS_SYNC_STATE_LOCKED) {

        tracker->consecutive_misses++;

        /* 6. 기준 횟수에 도달하면 동기 상실 */
        if (tracker->consecutive_misses >=
            tracker->config.loss_count) {

            tracker->state = FHSS_SYNC_STATE_SEARCHING;
            tracker->consecutive_misses = 0U;

            *out_event = FHSS_SYNC_EVENT_LOST;
        }
    }

    return FHSS_SYNC_STATUS_OK;
}


fhss_sync_status_t fhss_sync_state_get(
    const fhss_sync_state_tracker_t *tracker,
    fhss_sync_state_t *out_state
)
{
    /* 1. 출력 포인터 검사 */
    if (out_state == NULL) {
        return FHSS_SYNC_STATUS_INVALID_ARG;
    }

    /* 2. tracker 상태 검사 */
    const fhss_sync_status_t status = validate_tracker(tracker);

    if (status != FHSS_SYNC_STATUS_OK) {
        return status;
    }

    /* 3. 현재 상태 반환 */
    *out_state = tracker->state;

    return FHSS_SYNC_STATUS_OK;
}