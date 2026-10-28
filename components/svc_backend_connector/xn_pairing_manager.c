#include "xn_connector_internal.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "cJSON.h"
#include <string.h>

static const char *TAG = "xn_pairing";

static esp_websocket_client_handle_t pairing_ws_client = NULL;
static xn_device_info_t current_device_info = {0};

static void pairing_websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
    xn_connector_ctx_t *ctx = xn_connector_get_context();

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "Pairing WebSocket connected");

        // 发送 hello 消息
        cJSON *hello = cJSON_CreateObject();
        cJSON_AddStringToObject(hello, "type", "hello");
        cJSON_AddStringToObject(hello, "deviceUid", current_device_info.device_uid);
        cJSON_AddStringToObject(hello, "name", current_device_info.name);
        cJSON_AddStringToObject(hello, "firmwareVersion", current_device_info.firmware_version);

        cJSON *capabilities = cJSON_CreateArray();
        for (uint8_t i = 0; i < current_device_info.capabilities_count; i++) {
            cJSON_AddItemToArray(capabilities, cJSON_CreateString(current_device_info.capabilities[i]));
        }
        cJSON_AddItemToObject(hello, "capabilities", capabilities);

        char *hello_str = cJSON_PrintUnformatted(hello);
        if (hello_str != NULL) {
            esp_websocket_client_send_text(pairing_ws_client, hello_str, strlen(hello_str), portMAX_DELAY);
            free(hello_str);
        }
        cJSON_Delete(hello);
        break;

    case WEBSOCKET_EVENT_DATA:
        if (data->op_code == 0x01) { // 文本帧
            ESP_LOGI(TAG, "Received pairing message: %.*s", data->data_len, (char *)data->data_ptr);

            cJSON *json = cJSON_ParseWithLength((const char *)data->data_ptr, data->data_len);
            if (json != NULL) {
                cJSON *type_obj = cJSON_GetObjectItem(json, "type");
                if (type_obj != NULL && cJSON_IsString(type_obj)) {
                    const char *type = type_obj->valuestring;

                    if (strcmp(type, "pairing_pending") == 0) {
                        cJSON *pairing_id_obj = cJSON_GetObjectItem(json, "pairingId");
                        if (pairing_id_obj != NULL && cJSON_IsString(pairing_id_obj)) {
                            strncpy(ctx->pairing_id, pairing_id_obj->valuestring, sizeof(ctx->pairing_id) - 1);
                            ESP_LOGI(TAG, "Pairing pending, pairingId: %s", ctx->pairing_id);

                            if (ctx->callbacks.pairing_cb != NULL) {
                                ctx->callbacks.pairing_cb(XN_PAIRING_EVENT_PENDING, ctx->pairing_id, ctx->callbacks.user_data);
                            }
                        }
                    } else if (strcmp(type, "paired") == 0) {
                        cJSON *device_id_obj = cJSON_GetObjectItem(json, "deviceId");
                        cJSON *tenant_id_obj = cJSON_GetObjectItem(json, "tenantId");
                        cJSON *auth_code_obj = cJSON_GetObjectItem(json, "deviceAuthCode");

                        if (device_id_obj != NULL && tenant_id_obj != NULL && auth_code_obj != NULL &&
                            cJSON_IsString(device_id_obj) && cJSON_IsString(tenant_id_obj) && cJSON_IsString(auth_code_obj)) {

                            xn_auth_info_t auth_info = {0};
                            strncpy(auth_info.device_id, device_id_obj->valuestring, sizeof(auth_info.device_id) - 1);
                            strncpy(auth_info.tenant_id, tenant_id_obj->valuestring, sizeof(auth_info.tenant_id) - 1);
                            strncpy(auth_info.auth_code, auth_code_obj->valuestring, sizeof(auth_info.auth_code) - 1);
                            auth_info.is_valid = true;

                            esp_err_t ret = xn_auth_save(&auth_info);
                            if (ret == ESP_OK) {
                                memcpy(&ctx->auth_info, &auth_info, sizeof(xn_auth_info_t));
                                ctx->state = XN_CONNECTOR_STATE_PAIRED;

                                ESP_LOGI(TAG, "Pairing successful, deviceId: %s", auth_info.device_id);

                                if (ctx->callbacks.pairing_cb != NULL) {
                                    ctx->callbacks.pairing_cb(XN_PAIRING_EVENT_SUCCESS, NULL, ctx->callbacks.user_data);
                                }
                            } else {
                                ESP_LOGE(TAG, "Failed to save auth info");
                                if (ctx->callbacks.pairing_cb != NULL) {
                                    ctx->callbacks.pairing_cb(XN_PAIRING_EVENT_FAILED, NULL, ctx->callbacks.user_data);
                                }
                            }
                        }
                    } else if (strcmp(type, "error") == 0) {
                        cJSON *message_obj = cJSON_GetObjectItem(json, "message");
                        const char *message = (message_obj != NULL && cJSON_IsString(message_obj)) ?
                                              message_obj->valuestring : "Unknown error";
                        ESP_LOGE(TAG, "Pairing error: %s", message);

                        if (ctx->callbacks.pairing_cb != NULL) {
                            ctx->callbacks.pairing_cb(XN_PAIRING_EVENT_FAILED, NULL, ctx->callbacks.user_data);
                        }
                    }
                }
                cJSON_Delete(json);
            }
        }
        break;

    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "Pairing WebSocket error");
        if (ctx->callbacks.pairing_cb != NULL) {
            ctx->callbacks.pairing_cb(XN_PAIRING_EVENT_FAILED, NULL, ctx->callbacks.user_data);
        }
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "Pairing WebSocket disconnected");
        break;

    default:
        break;
    }
}

esp_err_t xn_pairing_start(const xn_device_info_t *device_info)
{
    if (device_info == NULL) {
        ESP_LOGE(TAG, "Device info is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    xn_connector_ctx_t *ctx = xn_connector_get_context();

    if (pairing_ws_client != NULL) {
        ESP_LOGW(TAG, "Pairing already started");
        return ESP_ERR_INVALID_STATE;
    }

    memcpy(&current_device_info, device_info, sizeof(xn_device_info_t));

    // 构建配对 WebSocket URL
    char pairing_url[512];
    snprintf(pairing_url, sizeof(pairing_url), "%s/ws/device/pair", ctx->config.server_url);

    esp_websocket_client_config_t ws_cfg = {
        .uri = pairing_url,
        .reconnect_timeout_ms = 5000,
        .network_timeout_ms = 10000,
    };

    pairing_ws_client = esp_websocket_client_init(&ws_cfg);
    if (pairing_ws_client == NULL) {
        ESP_LOGE(TAG, "Failed to init WebSocket client");
        return ESP_FAIL;
    }

    esp_websocket_register_events(pairing_ws_client, WEBSOCKET_EVENT_ANY, pairing_websocket_event_handler, NULL);

    esp_err_t ret = esp_websocket_client_start(pairing_ws_client);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start WebSocket client: %s", esp_err_to_name(ret));
        esp_websocket_client_destroy(pairing_ws_client);
        pairing_ws_client = NULL;
        return ret;
    }

    ctx->pairing_active = true;
    ESP_LOGI(TAG, "Pairing started");
    return ESP_OK;
}

esp_err_t xn_pairing_submit_code(const char *code)
{
    if (code == NULL) {
        ESP_LOGE(TAG, "Code is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    if (pairing_ws_client == NULL) {
        ESP_LOGE(TAG, "Pairing not started");
        return ESP_ERR_INVALID_STATE;
    }

    cJSON *pair_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(pair_msg, "type", "pair");
    cJSON_AddStringToObject(pair_msg, "code", code);

    char *msg_str = cJSON_PrintUnformatted(pair_msg);
    if (msg_str == NULL) {
        cJSON_Delete(pair_msg);
        ESP_LOGE(TAG, "Failed to create JSON message");
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = esp_websocket_client_send_text(pairing_ws_client, msg_str, strlen(msg_str), portMAX_DELAY);

    free(msg_str);
    cJSON_Delete(pair_msg);

    if (ret < 0) {
        ESP_LOGE(TAG, "Failed to send pairing code");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Pairing code submitted");
    return ESP_OK;
}

esp_err_t xn_pairing_stop(void)
{
    if (pairing_ws_client != NULL) {
        esp_websocket_client_stop(pairing_ws_client);
        esp_websocket_client_destroy(pairing_ws_client);
        pairing_ws_client = NULL;
    }

    xn_connector_ctx_t *ctx = xn_connector_get_context();
    ctx->pairing_active = false;
    memset(&current_device_info, 0, sizeof(xn_device_info_t));
    memset(ctx->pairing_id, 0, sizeof(ctx->pairing_id));

    ESP_LOGI(TAG, "Pairing stopped");
    return ESP_OK;
}
