#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Monitor-friendly wire diagnostics.  These helpers never mutate packets and
 * intentionally use one log tag (OTA_DIAG) so a captured monitor log can be
 * filtered without losing the RF -> protocol timeline. */
void fhss_ota_diag_log_packet(
    const char *direction,
    const char *path,
    uint8_t channel,
    const uint8_t *packet,
    size_t length);

void fhss_ota_diag_log_rx_result(
    const char *path,
    uint8_t channel,
    int status,
    bool crc_ok,
    int16_t rssi_dbm,
    uint8_t lqi,
    size_t length);

void fhss_ota_diag_log_tx_result(
    const char *path,
    uint8_t channel,
    int status,
    size_t length);

#ifdef __cplusplus
}
#endif
