#include "xn_connector_internal.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "cJSON.h"
#include <string.h>

static const char *TAG = "xn_connection";

static void heartbeat_task(void *arg);

static void connection_websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
    xn_connector_ctx_t *ctx = xn_connector_get_context();

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "WebSocket connected");

        xSemaphoreTake(ctx->state_mutex, portMAX_DELAY);
        ctx->state = XN_CONNECTOR_STATE_CONNECTED;
        xSemaphoreGive(ctx->state_mutex);

        if (ctx->callbacks.conn_cb != NULL) {
            ctx->callbacks.conn_cb(XN_CONN_EVENT_CONNECTED, ctx->callbacks.user_data);
        }

        // 启动心跳任务
        if (ctx->config.heartbeat_interval_ms > 0 && ctx->heartbeat_task == NULL) {
            xTaskCreate(heartbeat_task, "xn_heartbeat", 2048, NULL, 5, &ctx->heartbeat_task);
        }
        break;

    case WEBSOCKET_EVENT_DATA:
        xn_message_router_handle((const uint8_t *)data->data_ptr, data->data_len, data->op_code == 0x02);
        break;

    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "WebSocket error");

        xSemaphoreTake(ctx->state_mutex, portMAX_DELAY);
        ctx->state = XN_CONNECTOR_STATE_ERROR;
        xSemaphoreGive(ctx->state_mutex);

        if (ctx->callbacks.conn_cb != NULL) {
            ctx->callbacks.conn_cb(XN_CONN_EVENT_ERROR, ctx->callbacks.user_data);
        }
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "WebSocket disconnected");

        xSemaphoreTake(ctx->state_mutex, portMAX_DELAY);
        ctx->state = XN_CONNECTOR_STATE_DISCONNECTED;
        xSemaphoreGive(ctx->state_mutex);

        if (ctx->callbacks.conn_cb != NULL) {
            ctx->callbacks.conn_cb(XN_CONN_EVENT_DISCONNECTED, ctx->callbacks.user_data);
        }

        // 停止心跳任务
        if (ctx->heartbeat_task != NULL) {
            vTaskDelete(ctx->heartbeat_task);
            ctx->heartbeat_task = NULL;
        }

        // 自动重连
        if (ctx->config.auto_reconnect) {
            ESP_LOGI(TAG, "Auto reconnecting in %d ms", ctx->config.reconnect_interval_ms);
            vTaskDelay(pdMS_TO_TICKS(ctx->config.reconnect_interval_ms));

            if (ctx->callbacks.conn_cb != NULL) {
                ctx->callbacks.conn_cb(XN_CONN_EVENT_RECONNECTING, ctx->callbacks.user_data);
            }

            esp_websocket_client_start(ctx->ws_client);
        }
        break;

    default:
        break;
    }
}

static void heartbeat_task(void *arg)
{
    xn_connector_ctx_t *ctx = xn_connector_get_context();

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(ctx->config.heartbeat_interval_ms));

        if (ctx->state == XN_CONNECTOR_STATE_CONNECTED) {
            cJSON *ping = cJSON_CreateObject();
            cJSON_AddStringToObject(ping, "type", "ping");

            char *ping_str = cJSON_PrintUnformatted(ping);
            if (ping_str != NULL) {
                esp_websocket_client_send_text(ctx->ws_client, ping_str, strlen(ping_str), portMAX_DELAY);
                free(ping_str);
                ESP_LOGD(TAG, "Sent heartbeat ping");
            }
            cJSON_Delete(ping);
        } else {
            break;
        }
    }

    ctx->heartbeat_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t xn_connection_start(void)
{
    xn_connector_ctx_t *ctx = xn_connector_get_context();

    if (ctx->ws_client != NULL) {
        ESP_LOGW(TAG, "Connection already started");
        return ESP_ERR_INVALID_STATE;
    }

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

    // 构建 WebSocket URL
    char ws_url[512];
    snprintf(ws_url, sizeof(ws_url), "%s/ws/device?deviceId=%s&token=%s&voiceProtocol=%d",
             ctx->config.server_url, ctx->auth_info.device_id, device_token, ctx->config.voice_protocol);

    esp_websocket_client_config_t ws_cfg = {
        .uri = ws_url,
        .reconnect_timeout_ms = ctx->config.reconnect_interval_ms,
        .network_timeout_ms = 10000,
        .buffer_size = 8192,
    };

    ctx->ws_client = esp_websocket_client_init(&ws_cfg);
    if (ctx->ws_client == NULL) {
        ESP_LOGE(TAG, "Failed to init WebSocket client");
        return ESP_FAIL;
    }

    esp_websocket_register_events(ctx->ws_client, WEBSOCKET_EVENT_ANY, connection_websocket_event_handler, NULL);

    ret = esp_websocket_client_start(ctx->ws_client);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start WebSocket client: %s", esp_err_to_name(ret));
        esp_websocket_client_destroy(ctx->ws_client);
        ctx->ws_client = NULL;
        return ret;
    }

    ESP_LOGI(TAG, "Connection started");
    return ESP_OK;
}

esp_err_t xn_connection_stop(void)
{
    xn_connector_ctx_t *ctx = xn_connector_get_context();

    if (ctx->ws_client == NULL) {
        ESP_LOGW(TAG, "Connection not started");
        return ESP_OK;
    }

    // 停止心跳任务
    if (ctx->heartbeat_task != NULL) {
        vTaskDelete(ctx->heartbeat_task);
        ctx->heartbeat_task = NULL;
    }

    esp_websocket_client_stop(ctx->ws_client);
    esp_websocket_client_destroy(ctx->ws_client);
    ctx->ws_client = NULL;

    ESP_LOGI(TAG, "Connection stopped");
    return ESP_OK;
}

esp_err_t xn_connection_send_binary(const uint8_t *data, size_t len)
{
    xn_connector_ctx_t *ctx = xn_connector_get_context();

    if (ctx->ws_client == NULL) {
        ESP_LOGE(TAG, "Connection not started");
        return ESP_ERR_INVALID_STATE;
    }

    if (ctx->state != XN_CONNECTOR_STATE_CONNECTED) {
        ESP_LOGE(TAG, "Not connected");
        return ESP_ERR_INVALID_STATE;
    }

    int ret = esp_websocket_client_send_bin(ctx->ws_client, (const char *)data, len, portMAX_DELAY);
    if (ret < 0) {
        ESP_LOGE(TAG, "Failed to send binary data");
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t xn_connection_send_text(const char *text)
{
    xn_connector_ctx_t *ctx = xn_connector_get_context();

    if (ctx->ws_client == NULL) {
        ESP_LOGE(TAG, "Connection not started");
        return ESP_ERR_INVALID_STATE;
    }

    if (ctx->state != XN_CONNECTOR_STATE_CONNECTED) {
        ESP_LOGE(TAG, "Not connected");
        return ESP_ERR_INVALID_STATE;
    }

    int ret = esp_websocket_client_send_text(ctx->ws_client, text, strlen(text), portMAX_DELAY);
    if (ret < 0) {
        ESP_LOGE(TAG, "Failed to send text data");
        return ESP_FAIL;
    }

    return ESP_OK;
}
