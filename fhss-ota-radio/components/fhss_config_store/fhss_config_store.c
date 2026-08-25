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
    /* [2026-08-25, fix/fhss-config-store-resync] 이미 이 generation이
     * active로 승격되어 있으면 그대로 성공 처리한다 (멱등, idempotent —
     * 몇 번을 호출해도 결과가 같음을 뜻함).
     *
     * 왜 필요한가: fsm.c의 on_fhss_audio_event()는 SYNC_ACQUIRED
     * 이벤트를 받을 때마다(최초 동기화든, OTA_RECEIVING 도중 SYNC_LOST 후
     * 재동기화든 구분 없이) 매번 이 함수를 호출한다. 그런데 이 함수는
     * "pending" NVS 키를 "active"로 옮긴 뒤 pending을 지우는 1회성
     * 승격이라, 최초 승격 이후에는 pending이 이미 없어서 재호출 시 항상
     * ESP_ERR_INVALID_STATE를 반환했다. fsm.c는 이 에러를 무조건
     * 치명적으로 취급해 FSM_EVENT_ERROR -> ERROR 상태로 세션을 강제
     * 종료시켰다 — 재동기화 유예(resync grace, 10초)가 재동기화 자체는
     * 성공적으로 기다려줬는데도, 재동기화가 "성공하는" 바로 그 순간
     * 이 버그 때문에 세션이 죽는 구조였다.
     * (실기기 로그 artifacts/fhss5_slot_error2: SYNC_LOST 후 1.5초 만에
     * SYNC_ACQUIRED 성공 → 그 직후 "failed to promote synchronized FHSS
     * generation: ESP_ERR_INVALID_STATE" → OTA aborted at 18%)
     */
    ota_fhss_config_fields_t active = {0};
    if (fhss_config_store_load_active(&active) == ESP_OK &&
        active.generation == generation) {
        return ESP_OK;
    }

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
