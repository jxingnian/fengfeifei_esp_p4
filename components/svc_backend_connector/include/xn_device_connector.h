#ifndef XN_DEVICE_CONNECTOR_H
#define XN_DEVICE_CONNECTOR_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 设备连接器状态
 */
typedef enum {
    XN_CONNECTOR_STATE_IDLE = 0,        // 空闲状态
    XN_CONNECTOR_STATE_PAIRING,         // 配对中
    XN_CONNECTOR_STATE_PAIRED,          // 已配对
    XN_CONNECTOR_STATE_CONNECTING,      // 连接中
    XN_CONNECTOR_STATE_CONNECTED,       // 已连接
    XN_CONNECTOR_STATE_DISCONNECTED,    // 已断开
    XN_CONNECTOR_STATE_ERROR            // 错误状态
} xn_connector_state_t;

/**
 * 配对事件类型
 */
typedef enum {
    XN_PAIRING_EVENT_PENDING,           // 等待输入配对码
    XN_PAIRING_EVENT_SUCCESS,           // 配对成功
    XN_PAIRING_EVENT_FAILED,            // 配对失败
    XN_PAIRING_EVENT_TIMEOUT            // 配对超时
} xn_pairing_event_t;

/**
 * 连接事件类型
 */
typedef enum {
    XN_CONN_EVENT_CONNECTED,            // 已连接
    XN_CONN_EVENT_DISCONNECTED,         // 已断开
    XN_CONN_EVENT_RECONNECTING,         // 重连中
    XN_CONN_EVENT_ERROR                 // 连接错误
} xn_conn_event_t;

/**
 * 消息类型
 */
typedef enum {
    XN_MSG_TYPE_CONTROL,                // 控制消息（JSON）
    XN_MSG_TYPE_AUDIO,                  // 音频数据（二进制）
    XN_MSG_TYPE_ERROR                   // 错误消息
} xn_msg_type_t;

/**
 * 设备信息
 */
typedef struct {
    char device_uid[128];               // 设备唯一ID
    char name[80];                      // 设备名称
    char firmware_version[80];          // 固件版本
    char **capabilities;                // 能力列表
    uint8_t capabilities_count;         // 能力数量
} xn_device_info_t;

/**
 * 连接器配置
 */
typedef struct {
    char server_url[256];               // 服务器URL（ws://或wss://）
    uint16_t reconnect_interval_ms;     // 重连间隔（毫秒）
    uint16_t heartbeat_interval_ms;     // 心跳间隔（毫秒）
    uint32_t pairing_timeout_ms;        // 配对超时（毫秒）
    bool auto_reconnect;                // 是否自动重连
    uint8_t voice_protocol;             // 语音协议版本（1或2）
} xn_connector_config_t;

/**
 * 配对事件回调
 * @param event 配对事件类型
 * @param pairing_id 配对ID（仅在PENDING事件时有效）
 * @param user_data 用户数据
 */
typedef void (*xn_pairing_callback_t)(xn_pairing_event_t event, const char *pairing_id, void *user_data);

/**
 * 连接事件回调
 * @param event 连接事件类型
 * @param user_data 用户数据
 */
typedef void (*xn_conn_callback_t)(xn_conn_event_t event, void *user_data);

/**
 * 消息接收回调
 * @param msg_type 消息类型
 * @param data 消息数据
 * @param len 数据长度
 * @param user_data 用户数据
 */
typedef void (*xn_message_callback_t)(xn_msg_type_t msg_type, const uint8_t *data, size_t len, void *user_data);

/**
 * 事件回调集合
 */
typedef struct {
    xn_pairing_callback_t pairing_cb;   // 配对事件回调
    xn_conn_callback_t conn_cb;         // 连接事件回调
    xn_message_callback_t message_cb;   // 消息接收回调
    void *user_data;                    // 用户数据
} xn_connector_callbacks_t;

/**
 * 初始化设备连接器
 * @param config 连接器配置
 * @return ESP_OK 成功，其他值失败
 */
esp_err_t xn_connector_init(const xn_connector_config_t *config);

/**
 * 反初始化设备连接器
 * @return ESP_OK 成功，其他值失败
 */
esp_err_t xn_connector_deinit(void);

/**
 * 开始配对流程
 * @param device_info 设备信息
 * @param callbacks 事件回调
 * @return ESP_OK 成功，其他值失败
 */
esp_err_t xn_connector_start_pairing(const xn_device_info_t *device_info, const xn_connector_callbacks_t *callbacks);

/**
 * 提交配对码
 * @param pairing_code 6位数字配对码
 * @return ESP_OK 成功，其他值失败
 */
esp_err_t xn_connector_submit_pairing_code(const char *pairing_code);

/**
 * 停止配对
 * @return ESP_OK 成功，其他值失败
 */
esp_err_t xn_connector_stop_pairing(void);

/**
 * 连接到后台服务器
 * @param callbacks 事件回调
 * @return ESP_OK 成功，其他值失败
 */
esp_err_t xn_connector_connect(const xn_connector_callbacks_t *callbacks);

/**
 * 断开连接
 * @return ESP_OK 成功，其他值失败
 */
esp_err_t xn_connector_disconnect(void);

/**
 * 发送音频数据
 * @param audio_data 音频数据
 * @param len 数据长度
 * @return ESP_OK 成功，其他值失败
 */
esp_err_t xn_connector_send_audio(const uint8_t *audio_data, size_t len);

/**
 * 发送控制消息（JSON格式）
 * @param message JSON消息字符串
 * @return ESP_OK 成功，其他值失败
 */
esp_err_t xn_connector_send_control(const char *message);

/**
 * 获取当前状态
 * @return 连接器状态
 */
xn_connector_state_t xn_connector_get_state(void);

/**
 * 检查是否已配对
 * @return true 已配对，false 未配对
 */
bool xn_connector_is_paired(void);

/**
 * 检查是否已连接
 * @return true 已连接，false 未连接
 */
bool xn_connector_is_connected(void);

/**
 * 清除配对信息（取消配对）
 * @return ESP_OK 成功，其他值失败
 */
esp_err_t xn_connector_clear_pairing(void);

/**
 * 获取设备ID
 * @param device_id 输出设备ID缓冲区
 * @param len 缓冲区长度
 * @return ESP_OK 成功，其他值失败
 */
esp_err_t xn_connector_get_device_id(char *device_id, size_t len);

#ifdef __cplusplus
}
#endif

#endif // XN_DEVICE_CONNECTOR_H
