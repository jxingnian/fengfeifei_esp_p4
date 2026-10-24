#include "p4_c5_wifi.h"

#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"

static const char *TAG = "app_main";

/**
 * @brief 初始化保存 Wi-Fi 校准数据和系统参数所需的 NVS 分区。
 *
 * 当分区来自旧版本 ESP-IDF 或没有可用页时，按官方流程擦除后重新初始化，
 * 确保 P4 主程序和 C5 远程 Wi-Fi 驱动使用干净、可写的参数存储区。
 */
static void initialize_nvs(void)
{
    esp_err_t error = nvs_flash_init();
    if (error == ESP_ERR_NVS_NO_FREE_PAGES || error == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        error = nvs_flash_init();
    }
    ESP_ERROR_CHECK(error);
}

/**
 * @brief P4 应用入口。
 *
 * Wi-Fi 组件会立即启动 AP+STA 和网页服务器；STA 的实际联网由后台状态机
 * 异步完成，因此这里不能把 ESP_FAIL 当成整个网络模块初始化失败。
 */
void app_main(void)
{
    initialize_nvs();

    esp_err_t error = p4_c5_wifi_start();
    if (error == ESP_OK) {
        ESP_LOGI(TAG, "P4 Wi-Fi 配网组件已启动，可以启动 AI 语音、音视频和云服务任务");
        return;
    }

    ESP_LOGE(TAG, "P4/C5 Wi-Fi 配网组件初始化失败：%s", esp_err_to_name(error));
}
