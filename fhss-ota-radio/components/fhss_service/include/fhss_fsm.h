#pragma once

#include <stdbool.h>

typedef enum {
    FHSS_FSM_STATE_STOPPED = 0,
    FHSS_FSM_STATE_SEARCHING,
    FHSS_FSM_STATE_SYNCHRONIZING,
    FHSS_FSM_STATE_TRACKING,
    FHSS_FSM_STATE_TRANSMITTING,
} fhss_fsm_state_t;

typedef enum {
    FHSS_FSM_EVENT_START_RX = 0,
    FHSS_FSM_EVENT_START_TX,
    FHSS_FSM_EVENT_FIRST_SYNC,
    FHSS_FSM_EVENT_SYNC_ACQUIRED,
    FHSS_FSM_EVENT_SYNC_LOST,
    FHSS_FSM_EVENT_STOP,
} fhss_fsm_event_t;

typedef struct {
    fhss_fsm_state_t state;
} fhss_fsm_t;

void fhss_fsm_init(fhss_fsm_t *fsm);
bool fhss_fsm_handle(fhss_fsm_t *fsm, fhss_fsm_event_t event);
const char *fhss_fsm_state_name(fhss_fsm_state_t state);
