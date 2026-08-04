#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t ota_client_init(void);

#ifdef __cplusplus
}
#endif /* _OTA_CLIENT_H_ */