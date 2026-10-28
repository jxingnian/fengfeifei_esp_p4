#pragma once

/**
 * @file wifi_provisioning.h
 * @brief Wi-Fi 网页配网组件的公共接口。
 *
 * 本组件同时管理以下职责：
 * 1. 启动 ESP32-C5 的 AP+STA 双模式；
 * 2. 使用 NVS 持久化保存多个 Wi-Fi 配置；
 * 3. 开机和断网时轮询已保存的 Wi-Fi，每个 Wi-Fi 最多尝试三次；
 * 4. 通过 HTTP 网页提供扫描、保存、删除和状态查询接口；
 * 5. 向业务层提供稳定的连接状态和当前 IP 查询接口。
 *
 * 业务代码不应直接调用 esp_wifi_set_config()、esp_wifi_connect() 或
 * esp_wifi_scan_*()，避免业务层和配网状态机同时操作远程 Wi-Fi 驱动。
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 单条已保存的 Wi-Fi 配置。
 */
typedef struct {
    /** Wi-Fi 名称，最多 32 个字节，不包含结尾的字符串结束符。 */
    char ssid[33];
    /** Wi-Fi 密码，最多 64 个字节，不包含结尾的字符串结束符。 */
    char password[65];
} wifi_provisioning_profile_t;

/**
 * @brief Wi-Fi 配网运行状态。
 */
typedef enum {
    WIFI_PROVISIONING_STATE_STOPPED = 0,
    WIFI_PROVISIONING_STATE_STARTING,
    WIFI_PROVISIONING_STATE_CONNECTING,
    WIFI_PROVISIONING_STATE_CONNECTED,
    WIFI_PROVISIONING_STATE_PROVISIONING,
} wifi_provisioning_state_t;

/**
 * @brief 供网页和业务层使用的运行状态快照。
 */
typedef struct {
    /** 当前状态机状态。 */
    wifi_provisioning_state_t state;
    /** STA 是否已经获取 DHCP 地址。 */
    bool sta_connected;
    /** 当前连接使用的 SSID；未连接时为空字符串。 */
    char current_ssid[33];
    /** STA 当前 IPv4 地址。 */
    esp_netif_ip_info_t sta_ip_info;
    /** AP 接口当前 IPv4 地址。 */
    esp_netif_ip_info_t ap_ip_info;
    /** 本次启动已经完成的连接尝试次数。 */
    uint32_t attempt_count;
} wifi_provisioning_status_t;

/**
 * @brief 初始化并启动 AP+STA 双模式、HTTP 网页服务和连接管理任务。
 *
 * 函数返回 ESP_OK 只表示 Wi-Fi 驱动、AP 和 HTTP 服务已经启动；STA 是否
 * 联网由后台任务继续完成，调用者通过查询接口获取最终状态。这样即使没有
 * 保存 Wi-Fi，设备也能立即开启热点并允许手机进入配网页面。
 *
 * @return ESP_OK 初始化成功。
 * @return ESP_ERR_INVALID_STATE 已经启动过。
 * @return ESP_ERR_NO_MEM 内存不足。
 * @return 其他 ESP-IDF 错误码表示底层网络或 HTTP 服务初始化失败。
 */
esp_err_t wifi_provisioning_start(void);

/* Register optional local management routes before start. The array must have
 * static lifetime. Extensions must not depend on private Wi-Fi state. */
esp_err_t wifi_provisioning_register_web_extension(const httpd_uri_t *handlers, size_t count);

/**
 * @brief 等待 STA 成功获取 DHCP 地址。
 *
 * @param timeout_ticks 最大等待时间；传入 portMAX_DELAY 表示永久等待。
 * @return ESP_OK 在超时前已经联网。
 * @return ESP_ERR_TIMEOUT 在规定时间内没有联网。
 * @return ESP_ERR_INVALID_STATE 组件尚未启动。
 */
esp_err_t wifi_provisioning_wait_connected(TickType_t timeout_ticks);

/**
 * @brief 查询当前是否已经联网。
 *
 * @return true STA 已经获取有效 DHCP 地址。
 * @return false STA 尚未联网或已经断开。
 */
bool wifi_provisioning_is_connected(void);

/**
 * @brief 获取当前运行状态快照。
 *
 * @param[out] status 用于接收状态的有效指针。
 * @return ESP_OK 已成功复制状态。
 * @return ESP_ERR_INVALID_ARG status 为空。
 * @return ESP_ERR_INVALID_STATE 组件尚未启动。
 */
esp_err_t wifi_provisioning_get_status(wifi_provisioning_status_t *status);

/**
 * @brief 获取当前已保存的 Wi-Fi 配置列表。
 *
 * @param[out] profiles 用于接收配置数组的指针。
 * @param max_profiles profiles 数组的元素容量。
 * @param[out] profile_count 实际复制出的配置数量。
 * @return ESP_OK 成功获取。
 * @return ESP_ERR_INVALID_ARG 参数为空或容量为零。
 * @return ESP_ERR_INVALID_STATE 组件尚未启动。
 */
esp_err_t wifi_provisioning_get_profiles(wifi_provisioning_profile_t *profiles,
                                          size_t max_profiles,
                                          size_t *profile_count);

/**
 * @brief 保存或更新一条 Wi-Fi 配置，并立即触发连接尝试。
 *
 * 如果 SSID 已存在，函数会更新其密码并将该配置移动到轮询队列首位；如果
 * SSID 不存在，则插入队列首位，超过容量时删除队尾最旧配置。
 *
 * @param profile 待保存的配置。
 * @return ESP_OK 已写入 NVS 并已通知后台连接任务。
 * @return ESP_ERR_INVALID_ARG SSID 为空、字段超长或 profile 为空。
 * @return 其他错误码表示 NVS 写入失败。
 */
esp_err_t wifi_provisioning_save_profile(const wifi_provisioning_profile_t *profile);

/**
 * @brief 删除指定 SSID 的已保存配置。
 *
 * @param ssid 待删除的 SSID。
 * @return ESP_OK 删除成功，或 SSID 本来就不存在。
 * @return ESP_ERR_INVALID_ARG ssid 为空或超长。
 * @return 其他错误码表示 NVS 写入失败。
 */
esp_err_t wifi_provisioning_delete_profile(const char *ssid);

/**
 * @brief 手动通知后台重新轮询全部已保存 Wi-Fi。
 *
 * @return ESP_OK 已发送重连命令。
 * @return ESP_ERR_INVALID_STATE 组件尚未启动。
 */
esp_err_t wifi_provisioning_request_reconnect(void);

#ifdef __cplusplus
}
#endif
