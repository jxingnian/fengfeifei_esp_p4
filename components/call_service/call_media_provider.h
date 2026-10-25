#pragma once
#include "call_media.h"

/* Vendor implementations own credentials, SDK state and media resources.
 * The call worker only consumes this interface and provider-neutral states. */
typedef struct {
    const char *id;
    esp_err_t (*init)(void);
    esp_err_t (*connect_offer)(const char *offer_json);
    call_media_state_t (*state)(void);
    bool (*started)(void);
    bool (*failed)(void);
    void (*close)(void);
} call_media_provider_t;

extern const call_media_provider_t call_media_livekit_provider;
