#include "fhss_fsm.h"

#include <stdbool.h>
#include <stddef.h>

void fhss_fsm_init(fhss_fsm_t *fsm)
{
    if (fsm != NULL) {
        fsm->state = FHSS_FSM_STATE_STOPPED;
    }
}

bool fhss_fsm_handle(fhss_fsm_t *fsm, fhss_fsm_event_t event)
{
    if (fsm == NULL) {
        return false;
    }
    if (event == FHSS_FSM_EVENT_STOP) {
        fsm->state = FHSS_FSM_STATE_STOPPED;
        return true;
    }

    switch (fsm->state) {
    case FHSS_FSM_STATE_STOPPED:
        if (event == FHSS_FSM_EVENT_START_RX) {
            fsm->state = FHSS_FSM_STATE_SEARCHING;
            return true;
        }
        if (event == FHSS_FSM_EVENT_START_TX) {
            fsm->state = FHSS_FSM_STATE_TRANSMITTING;
            return true;
        }
        break;
    case FHSS_FSM_STATE_SEARCHING:
        if (event == FHSS_FSM_EVENT_FIRST_SYNC) {
            fsm->state = FHSS_FSM_STATE_SYNCHRONIZING;
            return true;
        }
        break;
    case FHSS_FSM_STATE_SYNCHRONIZING:
        if (event == FHSS_FSM_EVENT_SYNC_ACQUIRED) {
            fsm->state = FHSS_FSM_STATE_TRACKING;
            return true;
        }
        if (event == FHSS_FSM_EVENT_SYNC_LOST) {
            fsm->state = FHSS_FSM_STATE_SEARCHING;
            return true;
        }
        break;
    case FHSS_FSM_STATE_TRACKING:
        if (event == FHSS_FSM_EVENT_SYNC_DEGRADED) {
            fsm->state = FHSS_FSM_STATE_RECOVERY;
            return true;
        }
        if (event == FHSS_FSM_EVENT_SYNC_LOST) {
            fsm->state = FHSS_FSM_STATE_SEARCHING;
            return true;
        }
        break;
    case FHSS_FSM_STATE_RECOVERY:
        if (event == FHSS_FSM_EVENT_SYNC_RECOVERED) {
            fsm->state = FHSS_FSM_STATE_TRACKING;
            return true;
        }
        if (event == FHSS_FSM_EVENT_SYNC_LOST) {
            fsm->state = FHSS_FSM_STATE_SEARCHING;
            return true;
        }
        break;
    case FHSS_FSM_STATE_TRANSMITTING:
        break;
    default:
        break;
    }
    return false;
}

const char *fhss_fsm_state_name(fhss_fsm_state_t state)
{
    switch (state) {
    case FHSS_FSM_STATE_STOPPED: return "STOPPED";
    case FHSS_FSM_STATE_SEARCHING: return "SEARCHING";
    case FHSS_FSM_STATE_SYNCHRONIZING: return "SYNCHRONIZING";
    case FHSS_FSM_STATE_TRACKING: return "TRACKING";
    case FHSS_FSM_STATE_RECOVERY: return "RECOVERY";
    case FHSS_FSM_STATE_TRANSMITTING: return "TRANSMITTING";
    default: return "UNKNOWN";
    }
}
