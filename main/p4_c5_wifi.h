#pragma once

/**
 * @file p4_c5_wifi.h
 * @brief P4 业务层使用的 Wi-Fi 兼容接口。
 *
 * 具体的网页配网功能由 wifi_provisioning 组件提供；本头文件保留原有
 * p4_c5_wifi_* 名称，避免业务层因为组件替换而修改调用代码。
 */

#include <stdbool.h>

#include "esp_err.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动 P4-C5 ESP-Hosted 链路、AP+STA 和网页配网服务。
 *
 * 该函数不会等待 STA 联网完成；启动成功后后台任务会自动轮询已保存的
 * Wi-Fi，每个网络尝试三次，失败后保持热点并进入等待配网状态。
 */
esp_err_t p4_c5_wifi_start(void);

/**
 * @brief 在指定时间内等待 STA 获取 DHCP 地址。
 */
esp_err_t p4_c5_wifi_wait_connected(TickType_t timeout_ticks);

/**
 * @brief 查询 STA 当前是否已联网。
 */
bool p4_c5_wifi_is_connected(void);

/**
 * @brief 读取 STA 当前 IPv4 地址、掩码和网关。
 */
esp_err_t p4_c5_wifi_get_ip_info(esp_netif_ip_info_t *ip_info);

#ifdef __cplusplus
}
#endif
