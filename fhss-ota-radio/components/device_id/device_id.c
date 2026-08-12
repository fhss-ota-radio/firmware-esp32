#include "device_id.h"

#include <stdio.h>

#include "esp_mac.h"

void device_id_get(uint8_t out[DEVICE_ID_LEN])
{
    uint8_t mac[6];
    esp_efuse_mac_get_default(mac);

    for (int i = 0; i < DEVICE_ID_LEN; i++) {
        out[i] = mac[6 - DEVICE_ID_LEN + i];
    }
}

void device_id_get_hex(char *out, size_t out_capacity)
{
    uint8_t id[DEVICE_ID_LEN];
    device_id_get(id);

    char buf[DEVICE_ID_LEN * 2 + 1];
    for (int i = 0; i < DEVICE_ID_LEN; i++) {
        snprintf(&buf[i * 2], 3, "%02X", id[i]);
    }

    snprintf(out, out_capacity, "%s", buf);
}
