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
/* 旧常量保留兼容；实际分别使用下面的音频和控制队列容量。 */
#define CLOUD_GATEWAY_QUEUE_LENGTH 64
/* 音频是实时数据：积压时丢最旧帧，不能挤占控制消息。 */
#define CLOUD_GATEWAY_AUDIO_QUEUE_LENGTH 160
#define CLOUD_GATEWAY_CONTROL_QUEUE_LENGTH 64

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

/* 仅供本地配网页面使用；设备不会显示后台生成的配对码，只负责提交用户输入的配对码。 */
typedef struct {
    cloud_gateway_state_t state;
    bool paired;
    bool connected;
    /* 每次后台 WebSocket 断开后递增，业务层可据此拒绝沿用旧连接状态。 */
    uint32_t connection_epoch;
    char server_url[CLOUD_GATEWAY_URL_SIZE];
    char tenant_id[CLOUD_GATEWAY_TENANT_SIZE];
    char device_uid[40];
    char device_id[65];
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
/* 异步保存后台地址；客户归属只能由后台配对码决定，设备不再配置客户 ID。 */
esp_err_t cloud_gateway_configure(const char *server_url);
/* 异步把本地网页输入的后台配对码发送给临时 WebSocket。 */
esp_err_t cloud_gateway_pair(const char *pairing_code);
/* Clears local credentials only; does not revoke the backend record. */
esp_err_t cloud_gateway_forget(void);
/* Bounded, nonblocking enqueue, available only after ready. Never exposes auth. */
esp_err_t cloud_gateway_send(bool binary, const void *data, size_t length);
/* 丢弃尚未发送的旧上行音频；不影响控制消息或已在发送中的 WebSocket 帧。 */
void cloud_gateway_discard_audio(void);
/* Attach separate cloud routes/panel to the existing provisioning HTTP server. */
esp_err_t cloud_gateway_web_register(void);

#ifdef __cplusplus
}
#endif
