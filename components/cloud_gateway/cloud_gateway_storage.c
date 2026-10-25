#include "cloud_gateway_internal.h"
#include <string.h>
#include "nvs.h"
#include "sdkconfig.h"

/* A single versioned blob commits origin, ID and credential together. Never store
 * a code against a different server. Only the gateway worker accesses this API. */
esp_err_t cloud_gateway_storage_load(cloud_gateway_credentials_t *credentials)
{
    memset(credentials, 0, sizeof(*credentials));
    nvs_handle_t handle;
    esp_err_t error = nvs_open("cloud_gateway", NVS_READONLY, &handle);
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        credentials->version = 1;
        strlcpy(credentials->server_url, CONFIG_CLOUD_GATEWAY_SERVER_URL, sizeof(credentials->server_url));
        return cloud_gateway_valid_url(credentials->server_url) ? ESP_OK : ESP_ERR_INVALID_ARG;
    }
    if (error != ESP_OK) return error;
    size_t length = sizeof(*credentials);
    error = nvs_get_blob(handle, "identity", credentials, &length);
    nvs_close(handle);
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        memset(credentials, 0, sizeof(*credentials));
        credentials->version = 1;
        strlcpy(credentials->server_url, CONFIG_CLOUD_GATEWAY_SERVER_URL, sizeof(credentials->server_url));
        return cloud_gateway_valid_url(credentials->server_url) ? ESP_OK : ESP_ERR_INVALID_ARG;
    }
    if (error != ESP_OK) return error;
    if (length != sizeof(*credentials) || credentials->version != 1 ||
        !memchr(credentials->server_url, 0, sizeof(credentials->server_url)) ||
        !memchr(credentials->tenant_id, 0, sizeof(credentials->tenant_id)) ||
        !memchr(credentials->device_id, 0, sizeof(credentials->device_id)) ||
        !memchr(credentials->auth_code, 0, sizeof(credentials->auth_code)) ||
        !cloud_gateway_valid_url(credentials->server_url) ||
        (!!credentials->device_id[0] != !!credentials->auth_code[0])) return ESP_ERR_INVALID_STATE;
    return ESP_OK;
}

esp_err_t cloud_gateway_storage_save(const cloud_gateway_credentials_t *credentials)
{
    nvs_handle_t handle;
    esp_err_t error = nvs_open("cloud_gateway", NVS_READWRITE, &handle);
    if (error != ESP_OK) return error;
    error = nvs_set_blob(handle, "identity", credentials, sizeof(*credentials));
    if (error == ESP_OK) error = nvs_commit(handle);
    nvs_close(handle);
    return error;
}
