#include "xn_connector_internal.h"
#include "esp_log.h"
#include "cJSON.h"
#include <string.h>

static const char *TAG = "xn_msg_router";

esp_err_t xn_message_router_init(void)
{
    ESP_LOGI(TAG, "Message router initialized");
    return ESP_OK;
}

void xn_message_router_handle(const uint8_t *data, size_t len, bool is_binary)
{
    xn_connector_ctx_t *ctx = xn_connector_get_context();

    if (ctx->callbacks.message_cb == NULL) {
        ESP_LOGD(TAG, "No message callback registered");
        return;
    }

    if (is_binary) {
        // 二进制数据（音频）
        ESP_LOGD(TAG, "Received binary data: %d bytes", len);
        ctx->callbacks.message_cb(XN_MSG_TYPE_AUDIO, data, len, ctx->callbacks.user_data);
    } else {
        // 文本数据（控制消息）
        ESP_LOGD(TAG, "Received text data: %.*s", len, (char *)data);

        // 解析 JSON
        cJSON *json = cJSON_ParseWithLength((const char *)data, len);
        if (json != NULL) {
            cJSON *type_obj = cJSON_GetObjectItem(json, "type");
            if (type_obj != NULL && cJSON_IsString(type_obj)) {
                const char *type = type_obj->valuestring;

                // 处理特殊的系统消息
                if (strcmp(type, "pong") == 0) {
                    ESP_LOGD(TAG, "Received pong");
                } else if (strcmp(type, "error") == 0) {
                    cJSON *code_obj = cJSON_GetObjectItem(json, "code");
                    cJSON *message_obj = cJSON_GetObjectItem(json, "message");

                    const char *code = (code_obj != NULL && cJSON_IsString(code_obj)) ? code_obj->valuestring : "UNKNOWN";
                    const char *message = (message_obj != NULL && cJSON_IsString(message_obj)) ? message_obj->valuestring : "Unknown error";

                    ESP_LOGE(TAG, "Received error: %s - %s", code, message);

                    // 如果是认证撤销错误，需要清除配对信息
                    if (strcmp(code, "AUTH_REVOKED") == 0) {
                        ESP_LOGW(TAG, "Auth revoked, clearing pairing info");
                        xn_auth_clear();
                        ctx->auth_info.is_valid = false;

                        xSemaphoreTake(ctx->state_mutex, portMAX_DELAY);
                        ctx->state = XN_CONNECTOR_STATE_ERROR;
                        xSemaphoreGive(ctx->state_mutex);

                        if (ctx->callbacks.conn_cb != NULL) {
                            ctx->callbacks.conn_cb(XN_CONN_EVENT_ERROR, ctx->callbacks.user_data);
                        }
                    }

                    ctx->callbacks.message_cb(XN_MSG_TYPE_ERROR, data, len, ctx->callbacks.user_data);
                } else if (strcmp(type, "ready") == 0) {
                    ESP_LOGI(TAG, "Device ready event received");
                    ctx->callbacks.message_cb(XN_MSG_TYPE_CONTROL, data, len, ctx->callbacks.user_data);
                } else {
                    // 其他控制消息直接转发给应用层
                    ctx->callbacks.message_cb(XN_MSG_TYPE_CONTROL, data, len, ctx->callbacks.user_data);
                }
            }
            cJSON_Delete(json);
        } else {
            // 如果不是有效的 JSON，仍然转发
            ESP_LOGW(TAG, "Received non-JSON text data");
            ctx->callbacks.message_cb(XN_MSG_TYPE_CONTROL, data, len, ctx->callbacks.user_data);
        }
    }
}
