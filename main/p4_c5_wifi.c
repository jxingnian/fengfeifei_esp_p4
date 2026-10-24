#include "p4_c5_wifi.h"

#include "wifi_provisioning.h"

/**
 * @file p4_c5_wifi.c
 * @brief P4 业务层与通用 Wi-Fi 网页配网组件之间的适配层。
 *
 * 业务层继续使用原有的 p4_c5_wifi_* 接口，底层实现已经统一收敛到
 * wifi_provisioning 组件。这样 AI、音视频和云服务业务不需要了解 AP+STA、
 * NVS、网页服务器以及多 Wi-Fi 轮询的内部细节。
 */

esp_err_t p4_c5_wifi_start(void)
{
    return wifi_provisioning_start();
}

esp_err_t p4_c5_wifi_wait_connected(TickType_t timeout_ticks)
{
    return wifi_provisioning_wait_connected(timeout_ticks);
}

bool p4_c5_wifi_is_connected(void)
{
    return wifi_provisioning_is_connected();
}

esp_err_t p4_c5_wifi_get_ip_info(esp_netif_ip_info_t *ip_info)
{
    if (ip_info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    wifi_provisioning_status_t status;
    esp_err_t error = wifi_provisioning_get_status(&status);
    if (error != ESP_OK) {
        return error;
    }
    if (!status.sta_connected) {
        return ESP_ERR_INVALID_STATE;
    }

    *ip_info = status.sta_ip_info;
    return ESP_OK;
}
