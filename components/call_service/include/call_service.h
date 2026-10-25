#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef esp_err_t (*call_service_send_t)(bool binary, const void *data, size_t length);

esp_err_t call_service_init(call_service_send_t send);
void call_service_on_cloud_message(bool binary, const void *data, size_t length);

#ifdef __cplusplus
}
#endif
