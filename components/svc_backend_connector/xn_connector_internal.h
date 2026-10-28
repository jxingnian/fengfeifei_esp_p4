#ifndef XN_CONNECTOR_INTERNAL_H
#define XN_CONNECTOR_INTERNAL_H

#include "xn_device_connector.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"

#ifdef __cplusplus
extern "C" {
#endif

#define XN_NVS_NAMESPACE "xn_device"
#define XN_NVS_KEY_DEVICE_ID "device_id"
#define XN_NVS_KEY_AUTH_CODE "auth_code"
#define XN_NVS_KEY_TENANT_ID "tenant_id"

#define XN_MAX_DEVICE_ID_LEN 64
#define XN_MAX_AUTH_CODE_LEN 64
#define XN_MAX_TENANT_ID_LEN 64

/**
 * 认证信息
 */
typedef struct {
    char device_id[XN_MAX_DEVICE_ID_LEN];
    char auth_code[XN_MAX_AUTH_CODE_LEN];
    char tenant_id[XN_MAX_TENANT_ID_LEN];
    bool is_valid;
} xn_auth_info_t;

/**
 * 连接器上下文
 */
typedef struct {
    xn_connector_config_t config;
    xn_connector_state_t state;
    xn_connector_callbacks_t callbacks;
    xn_auth_info_t auth_info;

    esp_websocket_client_handle_t ws_client;
    SemaphoreHandle_t state_mutex;
    EventGroupHandle_t event_group;
    TaskHandle_t heartbeat_task;

    bool pairing_active;
    char pairing_id[64];
} xn_connector_ctx_t;

// 获取全局上下文
xn_connector_ctx_t *xn_connector_get_context(void);

// 认证管理模块
esp_err_t xn_auth_init(void);
esp_err_t xn_auth_save(const xn_auth_info_t *auth_info);
esp_err_t xn_auth_load(xn_auth_info_t *auth_info);
esp_err_t xn_auth_clear(void);
esp_err_t xn_auth_generate_token(const char *device_id, const char *auth_code, char *token, size_t token_len);

// 配对管理模块
esp_err_t xn_pairing_start(const xn_device_info_t *device_info);
esp_err_t xn_pairing_submit_code(const char *code);
esp_err_t xn_pairing_stop(void);

// 连接管理模块
esp_err_t xn_connection_start(void);
esp_err_t xn_connection_stop(void);
esp_err_t xn_connection_send_binary(const uint8_t *data, size_t len);
esp_err_t xn_connection_send_text(const char *text);

// 消息路由模块
esp_err_t xn_message_router_init(void);
void xn_message_router_handle(const uint8_t *data, size_t len, bool is_binary);

// 配置同步模块
esp_err_t xn_config_sync_init(void);
esp_err_t xn_config_sync_fetch(void);

#ifdef __cplusplus
}
#endif

#endif // XN_CONNECTOR_INTERNAL_H
