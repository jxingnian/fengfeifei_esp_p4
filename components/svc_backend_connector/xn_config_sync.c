#include "xn_connector_internal.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "cJSON.h"
#include <string.h>

static const char *TAG = "xn_config_sync";

#define CONFIG_BUFFER_SIZE 4096

esp_err_t xn_config_sync_init(void)
{
    ESP_LOGI(TAG, "Config sync initialized");
    return ESP_OK;
}

esp_err_t xn_config_sync_fetch(void)
{
    xn_connector_ctx_t *ctx = xn_connector_get_context();

    if (!ctx->auth_info.is_valid) {
        ESP_LOGE(TAG, "Device not paired");
        return ESP_ERR_INVALID_STATE;
    }

    // 生成 device token
    char device_token[128];
    esp_err_t ret = xn_auth_generate_token(ctx->auth_info.device_id, ctx->auth_info.auth_code,
                                           device_token, sizeof(device_token));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to generate device token");
        return ret;
    }

    // 构建配置 URL
    char config_url[512];
    snprintf(config_url, sizeof(config_url), "%s/api/device/config",
             ctx->config.server_url);

    // 准备请求头
    char auth_header[256];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", device_token);

    char device_id_header[128];
    snprintf(device_id_header, sizeof(device_id_header), "%s", ctx->auth_info.device_id);

    // 配置 HTTP 客户端
    esp_http_client_config_t http_config = {
        .url = config_url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 10000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&http_config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to init HTTP client");
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "Authorization", auth_header);
    esp_http_client_set_header(client, "X-Device-Id", device_id_header);

    // 执行请求
    ret = esp_http_client_perform(client);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(ret));
        esp_http_client_cleanup(client);
        return ret;
    }

    int status_code = esp_http_client_get_status_code(client);
    if (status_code != 200) {
        ESP_LOGE(TAG, "HTTP request failed with status code: %d", status_code);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    int content_length = esp_http_client_get_content_length(client);
    if (content_length <= 0 || content_length > CONFIG_BUFFER_SIZE) {
        ESP_LOGE(TAG, "Invalid content length: %d", content_length);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    // 读取响应数据
    char *buffer = malloc(CONFIG_BUFFER_SIZE);
    if (buffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate buffer");
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }

    int read_len = esp_http_client_read_response(client, buffer, CONFIG_BUFFER_SIZE - 1);
    esp_http_client_cleanup(client);

    if (read_len <= 0) {
        ESP_LOGE(TAG, "Failed to read response");
        free(buffer);
        return ESP_FAIL;
    }

    buffer[read_len] = '\0';
    ESP_LOGI(TAG, "Received config: %s", buffer);

    // 解析配置
    cJSON *json = cJSON_Parse(buffer);
    free(buffer);

    if (json == NULL) {
        ESP_LOGE(TAG, "Failed to parse config JSON");
        return ESP_FAIL;
    }

    // 提取配置信息
    cJSON *device_id_obj = cJSON_GetObjectItem(json, "deviceId");
    cJSON *tenant_id_obj = cJSON_GetObjectItem(json, "tenantId");
    cJSON *capabilities_obj = cJSON_GetObjectItem(json, "capabilities");
    cJSON *voice_provider_obj = cJSON_GetObjectItem(json, "voiceProvider");

    if (device_id_obj != NULL && cJSON_IsString(device_id_obj)) {
        ESP_LOGI(TAG, "Device ID: %s", device_id_obj->valuestring);
    }

    if (tenant_id_obj != NULL && cJSON_IsString(tenant_id_obj)) {
        ESP_LOGI(TAG, "Tenant ID: %s", tenant_id_obj->valuestring);
    }

    if (capabilities_obj != NULL && cJSON_IsArray(capabilities_obj)) {
        int cap_count = cJSON_GetArraySize(capabilities_obj);
        ESP_LOGI(TAG, "Capabilities count: %d", cap_count);
    }

    if (voice_provider_obj != NULL && cJSON_IsObject(voice_provider_obj)) {
        cJSON *kind_obj = cJSON_GetObjectItem(voice_provider_obj, "kind");
        if (kind_obj != NULL && cJSON_IsString(kind_obj)) {
            ESP_LOGI(TAG, "Voice provider kind: %s", kind_obj->valuestring);
        }
    }

    cJSON_Delete(json);

    ESP_LOGI(TAG, "Config fetched successfully");
    return ESP_OK;
}
