#pragma once

/**
 * @file backend_pairing.h
 * @brief 后台配对服务 - 设备与后台服务器配对连接
 *
 * 职责：
 * - 连接后台配对WebSocket (wss://host/ws/device/pair)
 * - 发送设备信息（deviceUid、名称、固件版本、能力）
 * - 接收配对码并通知应用层显示
 * - 接收配对结果并保存到NVS
 * - 管理配对状态和错误处理
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 配对事件类型
 */
typedef enum {
    BACKEND_PAIRING_EVENT_CONNECTING,      // 正在连接配对服务器
    BACKEND_PAIRING_EVENT_CONNECTED,       // 已连接到配对服务器
    BACKEND_PAIRING_EVENT_CODE_READY,      // 配对码已生成（需要显示到网页）
    BACKEND_PAIRING_EVENT_WAITING,         // 等待用户在后台输入配对码
    BACKEND_PAIRING_EVENT_PAIRED,          // 配对成功
    BACKEND_PAIRING_EVENT_ERROR,           // 配对错误
    BACKEND_PAIRING_EVENT_TIMEOUT,         // 配对超时
    BACKEND_PAIRING_EVENT_DISCONNECTED,    // 连接断开
} backend_pairing_event_type_t;

/**
 * @brief 配对结果数据
 */
typedef struct {
    char device_id[64];        // 设备ID
    char auth_code[128];       // 设备授权码
    char tenant_id[64];        // 客户ID
} backend_pairing_result_t;

/**
 * @brief 配对事件
 */
typedef struct {
    backend_pairing_event_type_t type;
    const char *pairing_code;              // 配对码（6位数字）
    int expires_in;                        // 配对码有效时间（秒）
    const backend_pairing_result_t *result; // 配对结果（仅在PAIRED事件有效）
    const char *error_message;             // 错误信息
} backend_pairing_event_t;

/**
 * @brief 配对事件回调函数
 */
typedef void (*backend_pairing_event_callback_t)(
    const backend_pairing_event_t *event,
    void *context
);

/**
 * @brief 配对配置
 */
typedef struct {
    const char *server_url;                // 后台配对WebSocket地址（wss://host/ws/device/pair）
    const char *device_uid;                // 设备唯一ID（通常使用MAC地址）
    const char *device_name;               // 设备名称
    const char *firmware_version;          // 固件版本
    const char **capabilities;             // 设备能力数组（如 ["microphone", "speaker", "camera"]）
    size_t capabilities_count;             // 能力数量
    backend_pairing_event_callback_t on_event; // 事件回调
    void *context;                         // 用户上下文
} backend_pairing_config_t;

/**
 * @brief 初始化配对服务
 *
 * @param config 配对配置
 * @return esp_err_t ESP_OK成功，其他失败
 */
esp_err_t backend_pairing_init(const backend_pairing_config_t *config);

/**
 * @brief 开始配对（连接WebSocket并发送hello消息）
 *
 * @return esp_err_t ESP_OK成功，其他失败
 */
esp_err_t backend_pairing_start(void);

/**
 * @brief 停止配对
 *
 * @return esp_err_t ESP_OK成功，其他失败
 */
esp_err_t backend_pairing_stop(void);

/**
 * @brief 获取当前配对码（用于网页显示）
 *
 * @return const char* 配对码字符串，如果未生成则返回NULL
 */
const char* backend_pairing_get_code(void);

/**
 * @brief 获取配对状态
 *
 * @return true 配对进行中，false 未配对或已完成
 */
bool backend_pairing_is_active(void);

/**
 * @brief 将配对结果保存到NVS
 *
 * @param result 配对结果
 * @return esp_err_t ESP_OK成功，其他失败
 */
esp_err_t backend_pairing_save_to_nvs(const backend_pairing_result_t *result);

/**
 * @brief 从NVS加载配对信息
 *
 * @param result 输出配对结果
 * @return esp_err_t ESP_OK成功且已配对，ESP_ERR_NVS_NOT_FOUND未配对，其他失败
 */
esp_err_t backend_pairing_load_from_nvs(backend_pairing_result_t *result);

/**
 * @brief 检查设备是否已配对
 *
 * @return true 已配对，false 未配对
 */
bool backend_pairing_is_paired(void);

/**
 * @brief 清除配对信息（用于重新配对）
 *
 * @return esp_err_t ESP_OK成功，其他失败
 */
esp_err_t backend_pairing_clear_nvs(void);

/**
 * @brief 清理配对服务资源
 *
 * @return esp_err_t ESP_OK成功，其他失败
 */
esp_err_t backend_pairing_deinit(void);

#ifdef __cplusplus
}
#endif
