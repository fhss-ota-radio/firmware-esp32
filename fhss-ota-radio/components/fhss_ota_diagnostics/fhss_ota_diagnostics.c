#include "fhss_ota_diagnostics.h"

#include <inttypes.h>

#include "esp_log.h"
#include "ota_protocol.h"

static const char *TAG = "OTA_DIAG";

static const char *packet_type_name(ota_packet_type_t type)
{
    switch (type) {
    case OTA_PKT_DISCOVER: return "DISCOVER";
    case OTA_PKT_DISCOVER_ACK: return "DISCOVER_ACK";
    case OTA_PKT_START: return "START";
    case OTA_PKT_DATA: return "DATA";
    case OTA_PKT_END: return "END";
    case OTA_PKT_ACK: return "ACK";
    case OTA_PKT_NACK: return "NACK";
    case OTA_PKT_FHSS_CONFIG: return "FHSS_CONFIG";
    case OTA_PKT_FHSS_ACTIVATE: return "FHSS_ACTIVATE";
    case OTA_PKT_FHSS_SYNC: return "FHSS_SYNC";
    default: return "UNKNOWN";
    }
}

void fhss_ota_diag_log_packet(
    const char *direction,
    const char *path,
    uint8_t channel,
    const uint8_t *packet,
    size_t length)
{
    ota_packet_type_t type = 0;
    const bool have_type = packet != NULL &&
        ota_protocol_peek_type(packet, length, &type);
    ESP_LOGI(TAG, "%s path=%s ch=%u type=%s(%u) bytes=%u",
             direction, path, channel,
             have_type ? packet_type_name(type) : "INVALID",
             have_type ? (unsigned)type : 0U, (unsigned)length);

    if (have_type && type == OTA_PKT_FHSS_CONFIG) {
        ota_fhss_config_fields_t fields = {0};
        if (ota_protocol_decode_fhss_config(packet, length, &fields)) {
            ESP_LOGI(TAG,
                     "CONFIG session=%" PRIu32 " target=%08" PRIX32
                     " gen=%" PRIu32 " alg=%u profile=%u first=%u count=%u"
                     " rendezvous=%u reserved=%u seed=%08" PRIX32
                     " slot_us=%" PRIu32 " guard_us=%" PRIu32,
                     fields.session_id, fields.target_device_id,
                     fields.generation, fields.algorithm_version,
                     fields.channel_profile_id, fields.first_channel,
                     fields.channel_count, fields.rendezvous_channel,
                     fields.reserved_channel, fields.seed,
                     fields.slot_duration_us,
                     fields.channel_switch_guard_us);
        } else {
            ESP_LOGW(TAG, "FHSS_CONFIG decode failed");
        }
    } else if (have_type && type == OTA_PKT_FHSS_ACTIVATE) {
        ota_fhss_activate_fields_t fields = {0};
        if (ota_protocol_decode_fhss_activate(packet, length, &fields)) {
            ESP_LOGI(TAG,
                     "ACTIVATE session=%" PRIu32 " target=%08" PRIX32
                     " gen=%" PRIu32,
                     fields.session_id, fields.target_device_id,
                     fields.generation);
        } else {
            ESP_LOGW(TAG, "FHSS_ACTIVATE decode failed");
        }
    } else if (have_type && type == OTA_PKT_FHSS_SYNC) {
        ota_fhss_sync_fields_t fields = {0};
        if (ota_protocol_decode_fhss_sync(packet, length, &fields)) {
            ESP_LOGI(TAG,
                     "SYNC gen=%" PRIu32 " seq=%u hop=%u slot=%" PRIu32,
                     fields.generation, fields.sequence, fields.hop_index,
                     fields.slot_number);
        } else {
            ESP_LOGW(TAG, "FHSS_SYNC decode failed");
        }
    } else if (have_type && (type == OTA_PKT_ACK || type == OTA_PKT_NACK)) {
        ota_ack_fields_t fields = {0};
        ota_packet_type_t response_type = 0;
        if (ota_protocol_decode_ack(
                packet, length, &response_type, &fields)) {
            ESP_LOGI(TAG,
                     "RESPONSE session=%" PRIu32 " for_type=%u seq=%" PRIu32
                     " result=%u",
                     fields.session_id, fields.acknowledged_type,
                     fields.sequence, fields.result_code);
        }
    }
    if (packet != NULL && length > 0U) {
        ESP_LOG_BUFFER_HEXDUMP(TAG, packet, length, ESP_LOG_INFO);
    }
}

void fhss_ota_diag_log_rx_result(
    const char *path,
    uint8_t channel,
    int status,
    bool crc_ok,
    int16_t rssi_dbm,
    uint8_t lqi,
    size_t length)
{
    ESP_LOGI(TAG,
             "RX_RESULT path=%s ch=%u status=%d crc=%u rssi=%d lqi=%u bytes=%u",
             path, channel, status, crc_ok ? 1U : 0U, (int)rssi_dbm,
             (unsigned)lqi, (unsigned)length);
}

void fhss_ota_diag_log_tx_result(
    const char *path,
    uint8_t channel,
    int status,
    size_t length)
{
    ESP_LOGI(TAG, "TX_RESULT path=%s ch=%u status=%d bytes=%u",
             path, channel, status, (unsigned)length);
}
