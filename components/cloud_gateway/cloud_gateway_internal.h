#pragma once
#include "cloud_gateway.h"

typedef struct {
    uint32_t version;
    char server_url[CLOUD_GATEWAY_URL_SIZE];
    char tenant_id[CLOUD_GATEWAY_TENANT_SIZE];
    char device_id[65];
    char auth_code[129];
} cloud_gateway_credentials_t;

bool cloud_gateway_valid_url(const char *url);
esp_err_t cloud_gateway_storage_load(cloud_gateway_credentials_t *credentials);
esp_err_t cloud_gateway_storage_save(const cloud_gateway_credentials_t *credentials);
