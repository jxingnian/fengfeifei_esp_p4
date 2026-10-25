#pragma once
#include "esp_err.h"
#include <stdbool.h>

typedef enum {
    CALL_MEDIA_DISCONNECTED, CALL_MEDIA_CONNECTING, CALL_MEDIA_CONNECTED, CALL_MEDIA_FAILED
} call_media_state_t;

/* All functions belong to the call worker. No LCD renderer. */
esp_err_t call_media_init(void);
esp_err_t call_media_connect(const char *provider, const char *offer_json);
call_media_state_t call_media_state(void);
bool call_media_started(void);
bool call_media_failed(void);
void call_media_close(void);
