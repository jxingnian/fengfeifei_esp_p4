#include "xn_connector_internal.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "mbedtls/md.h"
#include "mbedtls/base64.h"
#include <string.h>

static const char *TAG = "xn_auth";

esp_err_t xn_auth_init(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition was truncated and needs to be erased");
        ret = nvs_flash_erase();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to erase NVS: %s", esp_err_to_name(ret));
            return ret;
        }
        ret = nvs_flash_init();
    }

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init NVS: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Auth module initialized");
    return ESP_OK;
}

esp_err_t xn_auth_save(const xn_auth_info_t *auth_info)
{
    if (auth_info == NULL) {
        ESP_LOGE(TAG, "Auth info is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open(XN_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = nvs_set_str(nvs_handle, XN_NVS_KEY_DEVICE_ID, auth_info->device_id);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save device_id: %s", esp_err_to_name(ret));
        nvs_close(nvs_handle);
        return ret;
    }

    ret = nvs_set_str(nvs_handle, XN_NVS_KEY_AUTH_CODE, auth_info->auth_code);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save auth_code: %s", esp_err_to_name(ret));
        nvs_close(nvs_handle);
        return ret;
    }

    ret = nvs_set_str(nvs_handle, XN_NVS_KEY_TENANT_ID, auth_info->tenant_id);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save tenant_id: %s", esp_err_to_name(ret));
        nvs_close(nvs_handle);
        return ret;
    }

    ret = nvs_commit(nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit NVS: %s", esp_err_to_name(ret));
        nvs_close(nvs_handle);
        return ret;
    }

    nvs_close(nvs_handle);
    ESP_LOGI(TAG, "Auth info saved to NVS");
    return ESP_OK;
}

esp_err_t xn_auth_load(xn_auth_info_t *auth_info)
{
    if (auth_info == NULL) {
        ESP_LOGE(TAG, "Auth info is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    memset(auth_info, 0, sizeof(xn_auth_info_t));

    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open(XN_NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (ret != ESP_OK) {
        if (ret == ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "No auth info found in NVS");
        } else {
            ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(ret));
        }
        return ret;
    }

    size_t len = XN_MAX_DEVICE_ID_LEN;
    ret = nvs_get_str(nvs_handle, XN_NVS_KEY_DEVICE_ID, auth_info->device_id, &len);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to read device_id: %s", esp_err_to_name(ret));
        nvs_close(nvs_handle);
        return ret;
    }

    len = XN_MAX_AUTH_CODE_LEN;
    ret = nvs_get_str(nvs_handle, XN_NVS_KEY_AUTH_CODE, auth_info->auth_code, &len);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to read auth_code: %s", esp_err_to_name(ret));
        nvs_close(nvs_handle);
        return ret;
    }

    len = XN_MAX_TENANT_ID_LEN;
    ret = nvs_get_str(nvs_handle, XN_NVS_KEY_TENANT_ID, auth_info->tenant_id, &len);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to read tenant_id: %s", esp_err_to_name(ret));
        nvs_close(nvs_handle);
        return ret;
    }

    nvs_close(nvs_handle);

    auth_info->is_valid = true;
    ESP_LOGI(TAG, "Auth info loaded from NVS, device_id: %s", auth_info->device_id);
    return ESP_OK;
}

esp_err_t xn_auth_clear(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open(XN_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(ret));
        return ret;
    }

    nvs_erase_key(nvs_handle, XN_NVS_KEY_DEVICE_ID);
    nvs_erase_key(nvs_handle, XN_NVS_KEY_AUTH_CODE);
    nvs_erase_key(nvs_handle, XN_NVS_KEY_TENANT_ID);

    ret = nvs_commit(nvs_handle);
    nvs_close(nvs_handle);

    ESP_LOGI(TAG, "Auth info cleared from NVS");
    return ESP_OK;
}

esp_err_t xn_auth_generate_token(const char *device_id, const char *auth_code, char *token, size_t token_len)
{
    if (device_id == NULL || auth_code == NULL || token == NULL || token_len == 0) {
        ESP_LOGE(TAG, "Invalid arguments");
        return ESP_ERR_INVALID_ARG;
    }

    // 根据后台逻辑生成 deviceToken
    // signDeviceToken(deviceId, secret) - 使用 HMAC-SHA256

    unsigned char hmac_result[32];
    mbedtls_md_context_t ctx;
    mbedtls_md_type_t md_type = MBEDTLS_MD_SHA256;

    mbedtls_md_init(&ctx);
    esp_err_t ret = mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(md_type), 1);
    if (ret != 0) {
        ESP_LOGE(TAG, "Failed to setup HMAC: %d", ret);
        mbedtls_md_free(&ctx);
        return ESP_FAIL;
    }

    ret = mbedtls_md_hmac_starts(&ctx, (const unsigned char *)auth_code, strlen(auth_code));
    if (ret != 0) {
        ESP_LOGE(TAG, "Failed to start HMAC: %d", ret);
        mbedtls_md_free(&ctx);
        return ESP_FAIL;
    }

    ret = mbedtls_md_hmac_update(&ctx, (const unsigned char *)device_id, strlen(device_id));
    if (ret != 0) {
        ESP_LOGE(TAG, "Failed to update HMAC: %d", ret);
        mbedtls_md_free(&ctx);
        return ESP_FAIL;
    }

    ret = mbedtls_md_hmac_finish(&ctx, hmac_result);
    if (ret != 0) {
        ESP_LOGE(TAG, "Failed to finish HMAC: %d", ret);
        mbedtls_md_free(&ctx);
        return ESP_FAIL;
    }

    mbedtls_md_free(&ctx);

    // 转换为base64url格式
    size_t olen = 0;
    ret = mbedtls_base64_encode((unsigned char *)token, token_len, &olen, hmac_result, 32);
    if (ret != 0) {
        ESP_LOGE(TAG, "Failed to encode base64: %d", ret);
        return ESP_FAIL;
    }

    // 转换为base64url（替换字符并去除填充）
    for (size_t i = 0; i < olen; i++) {
        if (token[i] == '+') token[i] = '-';
        else if (token[i] == '/') token[i] = '_';
        else if (token[i] == '=') {
            token[i] = '\0';
            break;
        }
    }

    ESP_LOGD(TAG, "Generated device token");
    return ESP_OK;
}
