#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 ESP32-P4 到 ESP32-C5 的 ESP-Hosted 链路并启动 Wi-Fi Station。
 *
 * 本函数依次完成 TCP/IP 协议栈、事件循环、ESP-Hosted、Wi-Fi 驱动、
 * Station 参数和自动重连机制的初始化。函数会等待首次连接成功，或者等待
 * 首次重试次数耗尽。首次连接失败后，后台自动重连仍会继续运行。
 *
 * @return ESP_OK 首次连接成功并取得 DHCP 地址。
 * @return ESP_FAIL 首次重试次数耗尽，后台仍会继续重连。
 * @return 其他错误 初始化 ESP-Hosted、网络栈或 Wi-Fi 驱动失败。
 */
esp_err_t p4_c5_wifi_start(void);

/**
 * @brief 等待 Wi-Fi 获得有效的 DHCP 地址。
 *
 * @param timeout_ticks 最长等待时间；传入 portMAX_DELAY 表示永久等待。
 * @return ESP_OK 在超时前已经联网。
 * @return ESP_ERR_TIMEOUT 在指定时间内没有联网。
 * @return ESP_ERR_INVALID_STATE 网络模块尚未初始化。
 */
esp_err_t p4_c5_wifi_wait_connected(TickType_t timeout_ticks);

/**
 * @brief 查询当前是否已经取得 DHCP 地址。
 *
 * @return true 当前网络可用于上层 TCP、TLS、HTTP、WebSocket 等连接。
 * @return false 当前尚未取得地址或连接已经断开。
 */
bool p4_c5_wifi_is_connected(void);

/**
 * @brief 读取当前 Station 接口的 IPv4 地址、掩码和网关。
 *
 * @param[out] ip_info 用于接收当前 IPv4 信息的有效指针。
 * @return ESP_OK 已写入有效地址信息。
 * @return ESP_ERR_INVALID_ARG 参数为空。
 * @return ESP_ERR_INVALID_STATE 尚未联网。
 */
esp_err_t p4_c5_wifi_get_ip_info(esp_netif_ip_info_t *ip_info);

#ifdef __cplusplus
}
#endif
