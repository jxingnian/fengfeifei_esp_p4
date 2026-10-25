#pragma once
#include "esp_err.h"
#include "livekit.h"

/* All functions belong to the call worker. No LCD renderer. */
esp_err_t call_media_init(void);
esp_err_t call_media_connect(const char *url, const char *token);
livekit_connection_state_t call_media_state(void);
bool call_media_started(void);
bool call_media_failed(void);
void call_media_close(void);
