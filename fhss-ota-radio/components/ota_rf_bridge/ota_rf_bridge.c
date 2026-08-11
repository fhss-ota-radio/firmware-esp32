#include "ota_rf_bridge.h"

#include "ota_client.h"

_Static_assert(
    RF_TRANSPORT_MAX_PACKET_LENGTH == OTA_CLIENT_MAX_PACKET_LENGTH,
    "RF and OTA packet limits must stay aligned"
);

esp_err_t ota_rf_bridge_receive_once(
    const rf_transport_t *transport,
    uint32_t timeout_ms
)
{
    if (transport == NULL || timeout_ms == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    rf_transport_rx_packet_t packet = {0};
    const rf_transport_status_t status = rf_transport_receive_packet(
        transport,
        timeout_ms,
        &packet
    );

    if (status == RF_TRANSPORT_STATUS_TIMEOUT) {
        return ESP_ERR_TIMEOUT;
    }
    if (status != RF_TRANSPORT_STATUS_OK) {
        return ESP_FAIL;
    }
    if (!packet.crc_ok) {
        return ESP_ERR_INVALID_CRC;
    }
    if (packet.length == 0U ||
        packet.length > RF_TRANSPORT_MAX_PACKET_LENGTH ||
        packet.length > OTA_CLIENT_MAX_PACKET_LENGTH) {
        return ESP_ERR_INVALID_SIZE;
    }

    return ota_client_submit_packet(packet.payload, packet.length);
}
