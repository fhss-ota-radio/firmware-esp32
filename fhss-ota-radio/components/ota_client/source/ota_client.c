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