#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "ota_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t fhss_config_store_init(void);
esp_err_t fhss_config_store_save_pending(
    const ota_fhss_config_fields_t *config);
esp_err_t fhss_config_store_load_pending(
    ota_fhss_config_fields_t *config);
esp_err_t fhss_config_store_load_active(
    ota_fhss_config_fields_t *config);
esp_err_t fhss_config_store_activate(uint32_t generation);
esp_err_t fhss_config_store_clear_pending(void);

#ifdef __cplusplus
}
#endif
