/**
 * @file backend_pairing.c
 * @brief 后台配对服务实现 - WebSocket连接和配对流程管理
 */

#include "backend_pairing.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include <string.h>

static const char *TAG = "backend_pairing";

// NVS命名空间和键名
#define NVS_NAMESPACE "pairing"
#define NVS_KEY_DEVICE_ID "device_id"
#define NVS_KEY_AUTH_CODE "auth_code"
#define NVS_KEY_TENANT_ID "tenant_id"
#define NVS_KEY_PAIRED "is_paired"

// 配对状态
typedef enum {
    PAIRING_STATE_IDLE,
    PAIRING_STATE_CONNECTING,
    PAIRING_STATE_CONNECTED,
    PAIRING_STATE_WAITING_CODE,
    PAIRING_STATE_WAITING_PAIR,
    PAIRING_STATE_PAIRED,
    PAIRING_STATE_ERROR,
} pairing_state_t;

// 配对上下文
typedef struct {
    esp_websocket_client_handle_t ws_client;
    backend_pairing_config_t config;
    backend_pairing_event_callback_t callback;
    void *user_context;

    pairing_state_t state;
    char pairing_code[8];
    int code_expires_in;
    backend_pairing_result_t result;
    char error_message[256];

    bool initialized;
    bool active;
} pairing_context_t;

static pairing_context_t s_pairing_ctx = {0};

// 前向声明
static void websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data);
static void send_hello_message(void);
static void process_pairing_code_ready(cJSON *json);
static void process_paired(cJSON *json);
static void process_error(cJSON *json);
static void notify_event(backend_pairing_event_type_t type);

// ========== 公共API实现 ==========

esp_err_t backend_pairing_init(const backend_pairing_config_t *config)
{
    if (config == NULL || config->server_url == NULL ||
        config->device_uid == NULL || config->on_event == NULL) {
        ESP_LOGE(TAG, "配置参数无效");
        return ESP_ERR_INVALID_ARG;
    }

    if (s_pairing_ctx.initialized) {
        ESP_LOGW(TAG, "配对服务已初始化");
        return ESP_OK;
    }

    memset(&s_pairing_ctx, 0, sizeof(pairing_context_t));

    // 复制配置（需要深拷贝字符串）
    s_pairing_ctx.config.server_url = strdup(config->server_url);
    s_pairing_ctx.config.device_uid = strdup(config->device_uid);
    s_pairing_ctx.config.device_name = config->device_name ? strdup(config->device_name) : strdup("ESP32-Device");
    s_pairing_ctx.config.firmware_version = config->firmware_version ? strdup(config->firmware_version) : strdup("1.0.0");
    s_pairing_ctx.config.capabilities_count = config->capabilities_count;

    // 复制能力数组
    if (config->capabilities_count > 0 && config->capabilities) {
        char **caps = malloc(sizeof(char*) * config->capabilities_count);
        for (size_t i = 0; i < config->capabilities_count; i++) {
            caps[i] = strdup(config->capabilities[i]);
        }
        s_pairing_ctx.config.capabilities = (const char**)caps;
    }

    s_pairing_ctx.callback = config->on_event;
    s_pairing_ctx.user_context = config->context;
    s_pairing_ctx.state = PAIRING_STATE_IDLE;
    s_pairing_ctx.initialized = true;

    ESP_LOGI(TAG, "配对服务初始化完成");
    return ESP_OK;
}

esp_err_t backend_pairing_start(void)
{
    if (!s_pairing_ctx.initialized) {
        ESP_LOGE(TAG, "配对服务未初始化");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_pairing_ctx.active) {
        ESP_LOGW(TAG, "配对已在进行中");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "开始配对流程，连接: %s", s_pairing_ctx.config.server_url);

    // 配置WebSocket客户端
    esp_websocket_client_config_t ws_config = {
        .uri = s_pairing_ctx.config.server_url,
        .reconnect_timeout_ms = 5000,
        .network_timeout_ms = 10000,
        .ping_interval_sec = 30,  // 每30秒发送心跳ping，保持连接活跃
        .crt_bundle_attach = esp_crt_bundle_attach,  // 使用ESP-IDF内置证书库验证服务器
    };

    s_pairing_ctx.ws_client = esp_websocket_client_init(&ws_config);
    if (s_pairing_ctx.ws_client == NULL) {
        ESP_LOGE(TAG, "WebSocket客户端初始化失败");
        return ESP_FAIL;
    }

    // 注册WebSocket事件处理器
    esp_websocket_register_events(s_pairing_ctx.ws_client, WEBSOCKET_EVENT_ANY,
                                   websocket_event_handler, NULL);

    // 启动WebSocket连接
    esp_err_t err = esp_websocket_client_start(s_pairing_ctx.ws_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WebSocket启动失败: %s", esp_err_to_name(err));
        esp_websocket_client_destroy(s_pairing_ctx.ws_client);
        s_pairing_ctx.ws_client = NULL;
        return err;
    }

    s_pairing_ctx.state = PAIRING_STATE_CONNECTING;
    s_pairing_ctx.active = true;
    notify_event(BACKEND_PAIRING_EVENT_CONNECTING);

    return ESP_OK;
}

esp_err_t backend_pairing_stop(void)
{
    if (!s_pairing_ctx.active) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "停止配对");

    if (s_pairing_ctx.ws_client) {
        esp_websocket_client_stop(s_pairing_ctx.ws_client);
        esp_websocket_client_destroy(s_pairing_ctx.ws_client);
        s_pairing_ctx.ws_client = NULL;
    }

    s_pairing_ctx.active = false;
    s_pairing_ctx.state = PAIRING_STATE_IDLE;

    return ESP_OK;
}

esp_err_t backend_pairing_restart(void)
{
    ESP_LOGI(TAG, "重启配对流程");

    // 先停止当前配对（如果正在进行）
    backend_pairing_stop();

    // 清理旧的配对码
    s_pairing_ctx.pairing_code[0] = '\0';
    s_pairing_ctx.code_expires_in = 0;
    s_pairing_ctx.error_message[0] = '\0';

    // 延迟100ms确保资源释放
    vTaskDelay(pdMS_TO_TICKS(100));

    // 重新启动配对
    return backend_pairing_start();
}

const char* backend_pairing_get_code(void)
{
    if (s_pairing_ctx.pairing_code[0] == '\0') {
        return NULL;
    }
    return s_pairing_ctx.pairing_code;
}

bool backend_pairing_is_active(void)
{
    return s_pairing_ctx.active;
}

esp_err_t backend_pairing_save_to_nvs(const backend_pairing_result_t *result)
{
    if (result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "打开NVS失败: %s", esp_err_to_name(err));
        return err;
    }

    // 保存配对信息
    err = nvs_set_str(nvs_handle, NVS_KEY_DEVICE_ID, result->device_id);
    if (err == ESP_OK) {
        err = nvs_set_str(nvs_handle, NVS_KEY_AUTH_CODE, result->auth_code);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(nvs_handle, NVS_KEY_TENANT_ID, result->tenant_id);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(nvs_handle, NVS_KEY_PAIRED, 1);
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
    }

    nvs_close(nvs_handle);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "配对信息已保存到NVS");
    } else {
        ESP_LOGE(TAG, "保存配对信息失败: %s", esp_err_to_name(err));
    }

    return err;
}

esp_err_t backend_pairing_load_from_nvs(backend_pairing_result_t *result)
{
    if (result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        return err;
    }

    // 检查是否已配对
    uint8_t is_paired = 0;
    err = nvs_get_u8(nvs_handle, NVS_KEY_PAIRED, &is_paired);
    if (err != ESP_OK || is_paired == 0) {
        nvs_close(nvs_handle);
        return ESP_ERR_NVS_NOT_FOUND;
    }

    // 读取配对信息
    size_t len;

    len = sizeof(result->device_id);
    err = nvs_get_str(nvs_handle, NVS_KEY_DEVICE_ID, result->device_id, &len);
    if (err != ESP_OK) {
        nvs_close(nvs_handle);
        return err;
    }

    len = sizeof(result->auth_code);
    err = nvs_get_str(nvs_handle, NVS_KEY_AUTH_CODE, result->auth_code, &len);
    if (err != ESP_OK) {
        nvs_close(nvs_handle);
        return err;
    }

    len = sizeof(result->tenant_id);
    err = nvs_get_str(nvs_handle, NVS_KEY_TENANT_ID, result->tenant_id, &len);
    if (err != ESP_OK) {
        nvs_close(nvs_handle);
        return err;
    }

    nvs_close(nvs_handle);
    return ESP_OK;
}

bool backend_pairing_is_paired(void)
{
    backend_pairing_result_t result;
    return (backend_pairing_load_from_nvs(&result) == ESP_OK);
}

esp_err_t backend_pairing_clear_nvs(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        return err;
    }

    // 清除所有配对信息
    nvs_erase_key(nvs_handle, NVS_KEY_DEVICE_ID);
    nvs_erase_key(nvs_handle, NVS_KEY_AUTH_CODE);
    nvs_erase_key(nvs_handle, NVS_KEY_TENANT_ID);
    nvs_erase_key(nvs_handle, NVS_KEY_PAIRED);

    err = nvs_commit(nvs_handle);
    nvs_close(nvs_handle);

    ESP_LOGI(TAG, "配对信息已清除");
    return err;
}

esp_err_t backend_pairing_deinit(void)
{
    backend_pairing_stop();

    if (s_pairing_ctx.initialized) {
        // 释放配置字符串
        free((void*)s_pairing_ctx.config.server_url);
        free((void*)s_pairing_ctx.config.device_uid);
        free((void*)s_pairing_ctx.config.device_name);
        free((void*)s_pairing_ctx.config.firmware_version);

        // 释放能力数组
        if (s_pairing_ctx.config.capabilities) {
            for (size_t i = 0; i < s_pairing_ctx.config.capabilities_count; i++) {
                free((void*)s_pairing_ctx.config.capabilities[i]);
            }
            free((void*)s_pairing_ctx.config.capabilities);
        }

        s_pairing_ctx.initialized = false;
    }

    return ESP_OK;
}

// ========== 内部函数实现 ==========

static void websocket_event_handler(void *handler_args, esp_event_base_t base,
                                     int32_t event_id, void *event_data)
{
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "WebSocket已连接");
        s_pairing_ctx.state = PAIRING_STATE_CONNECTED;
        notify_event(BACKEND_PAIRING_EVENT_CONNECTED);

        // 发送hello消息
        send_hello_message();
        break;

    case WEBSOCKET_EVENT_DATA:
        if (data->data_len > 0 && data->op_code == 0x01) { // 文本帧
            char *json_str = strndup((char*)data->data_ptr, data->data_len);
            ESP_LOGD(TAG, "收到消息: %s", json_str);

            cJSON *json = cJSON_Parse(json_str);
            if (json != NULL) {
                cJSON *type = cJSON_GetObjectItem(json, "type");
                if (type && cJSON_IsString(type)) {
                    const char *type_str = type->valuestring;

                    if (strcmp(type_str, "pairing_code_ready") == 0) {
                        process_pairing_code_ready(json);
                    } else if (strcmp(type_str, "paired") == 0) {
                        process_paired(json);
                    } else if (strcmp(type_str, "error") == 0) {
                        process_error(json);
                    }
                }
                cJSON_Delete(json);
            }
            free(json_str);
        }
        break;

    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "WebSocket错误");
        s_pairing_ctx.state = PAIRING_STATE_ERROR;
        snprintf(s_pairing_ctx.error_message, sizeof(s_pairing_ctx.error_message),
                 "WebSocket连接错误");
        notify_event(BACKEND_PAIRING_EVENT_ERROR);
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "WebSocket断开");
        notify_event(BACKEND_PAIRING_EVENT_DISCONNECTED);
        break;

    default:
        break;
    }
}

static void send_hello_message(void)
{
    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "type", "hello");
    cJSON_AddStringToObject(json, "deviceUid", s_pairing_ctx.config.device_uid);
    cJSON_AddStringToObject(json, "name", s_pairing_ctx.config.device_name);
    cJSON_AddStringToObject(json, "firmwareVersion", s_pairing_ctx.config.firmware_version);

    // 添加能力数组
    cJSON *capabilities = cJSON_CreateArray();
    for (size_t i = 0; i < s_pairing_ctx.config.capabilities_count; i++) {
        cJSON_AddItemToArray(capabilities, cJSON_CreateString(s_pairing_ctx.config.capabilities[i]));
    }
    cJSON_AddItemToObject(json, "capabilities", capabilities);

    char *json_str = cJSON_PrintUnformatted(json);
    ESP_LOGI(TAG, "发送hello消息: %s", json_str);

    esp_websocket_client_send_text(s_pairing_ctx.ws_client, json_str, strlen(json_str), portMAX_DELAY);

    free(json_str);
    cJSON_Delete(json);

    s_pairing_ctx.state = PAIRING_STATE_WAITING_CODE;
}

static void process_pairing_code_ready(cJSON *json)
{
    cJSON *code = cJSON_GetObjectItem(json, "code");
    cJSON *expires_in = cJSON_GetObjectItem(json, "expiresIn");

    if (code && cJSON_IsString(code)) {
        strncpy(s_pairing_ctx.pairing_code, code->valuestring, sizeof(s_pairing_ctx.pairing_code) - 1);
        s_pairing_ctx.pairing_code[sizeof(s_pairing_ctx.pairing_code) - 1] = '\0';

        if (expires_in && cJSON_IsNumber(expires_in)) {
            s_pairing_ctx.code_expires_in = expires_in->valueint;
        }

        ESP_LOGI(TAG, "配对码已生成: %s (有效期: %d秒)",
                 s_pairing_ctx.pairing_code, s_pairing_ctx.code_expires_in);

        s_pairing_ctx.state = PAIRING_STATE_WAITING_PAIR;
        notify_event(BACKEND_PAIRING_EVENT_CODE_READY);
    }
}

static void process_paired(cJSON *json)
{
    cJSON *device_id = cJSON_GetObjectItem(json, "deviceId");
    cJSON *auth_code = cJSON_GetObjectItem(json, "deviceAuthCode");
    cJSON *tenant_id = cJSON_GetObjectItem(json, "tenantId");

    if (device_id && cJSON_IsString(device_id) &&
        auth_code && cJSON_IsString(auth_code) &&
        tenant_id && cJSON_IsString(tenant_id)) {

        strncpy(s_pairing_ctx.result.device_id, device_id->valuestring,
                sizeof(s_pairing_ctx.result.device_id) - 1);
        strncpy(s_pairing_ctx.result.auth_code, auth_code->valuestring,
                sizeof(s_pairing_ctx.result.auth_code) - 1);
        strncpy(s_pairing_ctx.result.tenant_id, tenant_id->valuestring,
                sizeof(s_pairing_ctx.result.tenant_id) - 1);

        ESP_LOGI(TAG, "配对成功！设备ID: %s", s_pairing_ctx.result.device_id);

        s_pairing_ctx.state = PAIRING_STATE_PAIRED;
        notify_event(BACKEND_PAIRING_EVENT_PAIRED);
    } else {
        ESP_LOGE(TAG, "配对响应格式错误");
        s_pairing_ctx.state = PAIRING_STATE_ERROR;
        snprintf(s_pairing_ctx.error_message, sizeof(s_pairing_ctx.error_message),
                 "配对响应格式错误");
        notify_event(BACKEND_PAIRING_EVENT_ERROR);
    }
}

static void process_error(cJSON *json)
{
    cJSON *message = cJSON_GetObjectItem(json, "message");
    const char *error_msg = (message && cJSON_IsString(message)) ?
                            message->valuestring : "未知错误";

    ESP_LOGE(TAG, "配对错误: %s", error_msg);

    strncpy(s_pairing_ctx.error_message, error_msg, sizeof(s_pairing_ctx.error_message) - 1);
    s_pairing_ctx.state = PAIRING_STATE_ERROR;
    notify_event(BACKEND_PAIRING_EVENT_ERROR);

    // 停止WebSocket连接，允许重新启动
    if (s_pairing_ctx.ws_client) {
        esp_websocket_client_stop(s_pairing_ctx.ws_client);
        esp_websocket_client_destroy(s_pairing_ctx.ws_client);
        s_pairing_ctx.ws_client = NULL;
    }
    s_pairing_ctx.active = false;
    ESP_LOGI(TAG, "WebSocket连接已清理，可以重新启动配对");
}

static void notify_event(backend_pairing_event_type_t type)
{
    if (s_pairing_ctx.callback == NULL) {
        return;
    }

    backend_pairing_event_t event = {
        .type = type,
        .pairing_code = s_pairing_ctx.pairing_code,
        .expires_in = s_pairing_ctx.code_expires_in,
        .result = (type == BACKEND_PAIRING_EVENT_PAIRED) ? &s_pairing_ctx.result : NULL,
        .error_message = (type == BACKEND_PAIRING_EVENT_ERROR) ? s_pairing_ctx.error_message : NULL,
    };

    s_pairing_ctx.callback(&event, s_pairing_ctx.user_context);
}
