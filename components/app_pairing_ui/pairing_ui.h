#pragma once

/**
 * @file pairing_ui.h
 * @brief 配对界面 - 本地HTTP服务器显示配对码
 *
 * 职责：
 * - 启动本地HTTP服务器（端口80）
 * - 显示配对码网页
 * - 更新配对状态
 * - 显示配对成功页面
 */

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动配对网页服务器
 *
 * @return esp_err_t ESP_OK成功，其他失败
 */
esp_err_t pairing_ui_start(void);

/**
 * @brief 设置配对模式标志
 *
 * @param is_pairing_mode true为配对模式，false为正常模式
 */
void pairing_ui_set_mode(bool is_pairing_mode);

/**
 * @brief 更新配对码显示
 *
 * @param code 配对码（6位数字）
 * @param expires_in 有效时间（秒）
 * @return esp_err_t ESP_OK成功，其他失败
 */
esp_err_t pairing_ui_update_code(const char *code, int expires_in);

/**
 * @brief 显示配对成功页面
 *
 * @return esp_err_t ESP_OK成功，其他失败
 */
esp_err_t pairing_ui_show_success(void);

/**
 * @brief 显示配对错误页面
 *
 * @param error_message 错误信息
 * @return esp_err_t ESP_OK成功，其他失败
 */
esp_err_t pairing_ui_show_error(const char *error_message);

/**
 * @brief 停止配对网页服务器
 *
 * @return esp_err_t ESP_OK成功，其他失败
 */
esp_err_t pairing_ui_stop(void);

/**
 * @brief 检查服务器是否正在运行
 *
 * @return true 正在运行，false 未运行
 */
bool pairing_ui_is_running(void);

#ifdef __cplusplus
}
#endif
