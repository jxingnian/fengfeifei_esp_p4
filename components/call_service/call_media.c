#include "call_media_provider.h"
#include <string.h>

static const call_media_provider_t *s_provider;
/* Register future providers here and advertise them in device_capabilities.
 * Adding a backend provider never implies a device SDK is available. */
static const call_media_provider_t *find_provider(const char *id)
{
    const call_media_provider_t *providers[] = { &call_media_livekit_provider };
    for (size_t i = 0; i < sizeof(providers) / sizeof(providers[0]); ++i)
        if (!strcmp(id, providers[i]->id)) return providers[i];
    return NULL;
}

esp_err_t call_media_init(void) { return call_media_livekit_provider.init(); }
esp_err_t call_media_connect(const char *provider, const char *offer_json)
{
    if (s_provider) return ESP_ERR_INVALID_STATE;
    const call_media_provider_t *impl = find_provider(provider);
    if (!impl) return ESP_ERR_NOT_SUPPORTED;
    s_provider = impl;
    esp_err_t error = impl->connect_offer(offer_json);
    if (error != ESP_OK) { impl->close(); s_provider = NULL; }
    return error;
}
call_media_state_t call_media_state(void) { return s_provider ? s_provider->state() : CALL_MEDIA_DISCONNECTED; }
bool call_media_started(void) { return s_provider && s_provider->started(); }
bool call_media_failed(void) { return s_provider && s_provider->failed(); }
void call_media_close(void) { if (s_provider) s_provider->close(); s_provider = NULL; }
