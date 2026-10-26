#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CLOUD_GATEWAY_URL_SIZE 192
#define CLOUD_GATEWAY_TENANT_SIZE 65
#define CLOUD_GATEWAY_MAX_MESSAGE 16384

typedef enum {
    CLOUD_GATEWAY_WAIT_NETWORK,
    CLOUD_GATEWAY_SYNC_TIME,
    CLOUD_GATEWAY_CONNECTING,
    CLOUD_GATEWAY_WAIT_CODE,
    /* 保留旧状态编号，避免本地状态页面和外部监控的数值协议发生变化。 */
    CLOUD_GATEWAY_PAIRING,
    CLOUD_GATEWAY_AUTHORIZED,
    CLOUD_GATEWAY_READY,
    CLOUD_GATEWAY_RETRY,
    CLOUD_GATEWAY_ERROR,
} cloud_gateway_state_t;

/* 仅供本地配网页面使用；pairing_code 是一次性临时授权码，长期授权码不在状态结构中。 */
typedef struct {
    cloud_gateway_state_t state;
    bool paired;
    bool connected;
    char server_url[CLOUD_GATEWAY_URL_SIZE];
    char tenant_id[CLOUD_GATEWAY_TENANT_SIZE];
    char device_uid[40];
    char device_id[65];
    char pairing_id[65];
    /* 临时授权码仅用于设备本地显示，长期授权码不会进入状态结构。 */
    char pairing_code[7];
    char message[192];
    uint32_t retry_seconds;
} cloud_gateway_status_t;

/* Called on the gateway worker task, only for the authenticated connection.
 * Data is borrowed until return. Copy/enqueue it to a business task; do not block,
 * call networking, or do camera/audio processing inside this callback.
 * Text events include ready/audio_start/audio_stop/ai_event, etc.; binary is
 * currently audio. Future camera signaling must use an agreed backend protocol.
 */
typedef void (*cloud_gateway_message_callback_t)(bool binary, const void *data,
                                                size_t length, void *context);
typedef struct {
    cloud_gateway_message_callback_t on_message;
    void *context;
} cloud_gateway_config_t;

/* NVS, network stack and wifi_provisioning must already be initialized.
 * Starts once; owns reconnects and TLS. Independent of voice/camera components. */
esp_err_t cloud_gateway_start(const cloud_gateway_config_t *config);
esp_err_t cloud_gateway_get_status(cloud_gateway_status_t *status);
/* Asynchronous commands; ESP_OK means queued, inspect status for final result.
 * Changing server/tenant while paired is rejected: explicitly forget first. */
esp_err_t cloud_gateway_configure(const char *server_url, const char *tenant_id);
/* Clears local credentials only; does not revoke the backend record. */
esp_err_t cloud_gateway_forget(void);
/* Bounded, nonblocking enqueue, available only after ready. Never exposes auth. */
esp_err_t cloud_gateway_send(bool binary, const void *data, size_t length);
/* Attach separate cloud routes/panel to the existing provisioning HTTP server. */
esp_err_t cloud_gateway_web_register(void);

#ifdef __cplusplus
}
#endif
