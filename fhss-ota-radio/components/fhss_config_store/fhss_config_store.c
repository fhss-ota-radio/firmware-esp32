#include "fhss_config_store.h"

#include <string.h>

#include "nvs.h"
#include "nvs_flash.h"

#define FHSS_CONFIG_NAMESPACE "fhss_cfg"
#define FHSS_PENDING_KEY      "pending"
#define FHSS_ACTIVE_KEY       "active"
#define FHSS_STORED_MAGIC     0x46484346U
#define FHSS_STORED_VERSION   1U

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    ota_fhss_config_fields_t fields;
} fhss_stored_config_t;

static bool config_is_supported(const ota_fhss_config_fields_t *config)
{
    return ota_fhss_config_is_valid(config) &&
           config->algorithm_version == OTA_FHSS_ALGORITHM_VERSION &&
           config->rendezvous_channel == 1U &&
           config->first_channel == 1U &&
           config->reserved_channel == 0U;
}

static esp_err_t open_store(nvs_open_mode_t mode, nvs_handle_t *handle)
{
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return nvs_open(FHSS_CONFIG_NAMESPACE, mode, handle);
}

static esp_err_t load_key(const char *key, ota_fhss_config_fields_t *config)
{
    if (key == NULL || config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t handle = 0;
    esp_err_t err = open_store(NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }
    fhss_stored_config_t stored = {0};
    size_t length = sizeof(stored);
    err = nvs_get_blob(handle, key, &stored, &length);
    nvs_close(handle);
    if (err != ESP_OK) {
        return err;
    }
    if (length != sizeof(stored) || stored.magic != FHSS_STORED_MAGIC ||
        stored.version != FHSS_STORED_VERSION || stored.size != sizeof(stored) ||
        !config_is_supported(&stored.fields)) {
        return ESP_ERR_INVALID_STATE;
    }
    *config = stored.fields;
    return ESP_OK;
}

esp_err_t fhss_config_store_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        return ESP_ERR_INVALID_STATE;
    }
    return err;
}

esp_err_t fhss_config_store_save_pending(
    const ota_fhss_config_fields_t *config)
{
    if (!config_is_supported(config)) {
        return ESP_ERR_INVALID_ARG;
    }
    const fhss_stored_config_t stored = {
        .magic = FHSS_STORED_MAGIC,
        .version = FHSS_STORED_VERSION,
        .size = sizeof(fhss_stored_config_t),
        .fields = *config,
    };
    nvs_handle_t handle = 0;
    esp_err_t err = open_store(NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_blob(handle, FHSS_PENDING_KEY, &stored, sizeof(stored));
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    if (handle != 0U) {
        nvs_close(handle);
    }
    if (err != ESP_OK) {
        return err;
    }
    ota_fhss_config_fields_t verified = {0};
    err = load_key(FHSS_PENDING_KEY, &verified);
    return err == ESP_OK && memcmp(&verified, config, sizeof(verified)) == 0
        ? ESP_OK
        : ESP_ERR_INVALID_STATE;
}

esp_err_t fhss_config_store_load_pending(ota_fhss_config_fields_t *config)
{
    return load_key(FHSS_PENDING_KEY, config);
}

esp_err_t fhss_config_store_load_active(ota_fhss_config_fields_t *config)
{
    return load_key(FHSS_ACTIVE_KEY, config);
}

esp_err_t fhss_config_store_activate(uint32_t generation)
{
    ota_fhss_config_fields_t pending = {0};
    esp_err_t err = fhss_config_store_load_pending(&pending);
    if (err != ESP_OK || pending.generation != generation) {
        return ESP_ERR_INVALID_STATE;
    }
    const fhss_stored_config_t stored = {
        .magic = FHSS_STORED_MAGIC,
        .version = FHSS_STORED_VERSION,
        .size = sizeof(fhss_stored_config_t),
        .fields = pending,
    };
    nvs_handle_t handle = 0;
    err = open_store(NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_blob(handle, FHSS_ACTIVE_KEY, &stored, sizeof(stored));
    }
    if (err == ESP_OK) {
        err = nvs_erase_key(handle, FHSS_PENDING_KEY);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    if (handle != 0U) {
        nvs_close(handle);
    }
    return err;
}

esp_err_t fhss_config_store_clear_pending(void)
{
    nvs_handle_t handle = 0;
    esp_err_t err = open_store(NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_erase_key(handle, FHSS_PENDING_KEY);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;
        }
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    if (handle != 0U) {
        nvs_close(handle);
    }
    return err;
}
