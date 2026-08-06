#include <string.h>

#include "ota_client.h"
#include "ota_client_internal.h"

static ota_client_context_t s_ota_client;

esp_err_t ota_client_init(const ota_client_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (config->send_callback == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (config->receive_timeout_ms == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_ota_client.state != OTA_CLIENT_STATE_UNINITIALIZED) {
        return ESP_ERR_INVALID_STATE;
    }

    memset(&s_ota_client, 0, sizeof(s_ota_client));

    s_ota_client.config = *config;
    s_ota_client.state = OTA_CLIENT_STATE_IDLE;

    return ESP_OK;
}

ota_client_state_t ota_client_get_state(void)
{
    return s_ota_client.state;
}

esp_err_t ota_client_start_session(
    uint32_t session_id,
    uint32_t image_size,
    uint32_t total_chunks
) {
    if (image_size == 0 || total_chunks == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_ota_client.state != OTA_CLIENT_STATE_IDLE) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = ota_writer_begin(&s_ota_client.writer, image_size);

    if (err != ESP_OK) {
        s_ota_client.state = OTA_CLIENT_STATE_ERROR;

        if (s_ota_client.config.event_callback != NULL) {
            s_ota_client.config.event_callback(
                OTA_CLIENT_EVENT_FAILED,
                0,
                err,
                s_ota_client.config.callback_context
            );
        }

        return err;
    }     

    s_ota_client.session_id = session_id;
    s_ota_client.image_size = image_size;
    s_ota_client.received_bytes = 0;
    s_ota_client.total_chunks = total_chunks;
    s_ota_client.expected_sequence = 0;
    s_ota_client.last_packet_tick = xTaskGetTickCount();
    s_ota_client.state = OTA_CLIENT_STATE_RECEIVING;

    if (s_ota_client.config.event_callback != NULL) {
        s_ota_client.config.event_callback(
            OTA_CLIENT_EVENT_STARTED,
            0,
            ESP_OK,
            s_ota_client.config.callback_context
        );
    }

    return ESP_OK;
}

esp_err_t ota_client_write_chunk(
    uint32_t session_id,
    uint32_t sequence,
    const uint8_t *data,
    size_t data_size
)
{
    if (data == NULL || data_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_ota_client.state != OTA_CLIENT_STATE_RECEIVING) {
        return ESP_ERR_INVALID_STATE;
    }

    if (session_id != s_ota_client.session_id) {
        return ESP_ERR_INVALID_ARG;
    }

    if (sequence != s_ota_client.expected_sequence ||
        sequence >= s_ota_client.total_chunks) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = ota_writer_write(
        &s_ota_client.writer,
        data,
        data_size
    );

    if (err != ESP_OK) {
        ota_writer_abort(&s_ota_client.writer);
        s_ota_client.state = OTA_CLIENT_STATE_ERROR;

        if (s_ota_client.config.event_callback != NULL) {
            s_ota_client.config.event_callback(
                OTA_CLIENT_EVENT_FAILED,
                0,
                err,
                s_ota_client.config.callback_context
            );
        }

        return err;
    }

    s_ota_client.received_bytes += data_size;
    s_ota_client.expected_sequence++;
    s_ota_client.last_packet_tick = xTaskGetTickCount();

    uint32_t progress = (uint32_t)(
        ((uint64_t)s_ota_client.received_bytes * 100U) /
        s_ota_client.image_size
    );

    if (s_ota_client.config.event_callback != NULL) {
        s_ota_client.config.event_callback(
            OTA_CLIENT_EVENT_PROGRESS,
            progress,
            ESP_OK,
            s_ota_client.config.callback_context
        );
    }

    return ESP_OK;
}