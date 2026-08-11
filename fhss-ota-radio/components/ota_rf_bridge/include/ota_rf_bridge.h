#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "rf_transport.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * CC1101 패킷 하나를 수신해 OTA client 큐에 값 복사한다.
 * transport의 생성/채널 설정/소유권은 호출자(FSM/FHSS 서비스)에 있다.
 */
esp_err_t ota_rf_bridge_receive_once(
    const rf_transport_t *transport,
    uint32_t timeout_ms
);

#ifdef __cplusplus
}
#endif
