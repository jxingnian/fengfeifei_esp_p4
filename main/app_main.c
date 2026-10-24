/*
 * @file app_main.c
 * @brief ESP32-P4 主业务入口。
 *
 * 启动顺序固定为：初始化 NVS、启动 P4/C5 Wi-Fi 配网组件、初始化并启动
 * “你好小飞”语音唤醒组件。Wi-Fi 联网状态机异步运行，因此语音服务不等待
 * STA 联网成功即可开始监听。
 */
#include "p4_c5_wifi.h"
#include "voice_service.h"

#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"

static const char *TAG = "app_main";

/**
 * @brief 初始化保存 Wi-Fi 配置和系统参数所需的 NVS 分区。
 *
 * 当分区来自旧版本 ESP-IDF 或没有可用页时，按照 ESP-IDF 官方流程擦除后
 * 重新初始化，确保 P4 主程序和 C5 远程 Wi-Fi 驱动可以正常保存参数。
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
 * @brief 处理语音组件事件。
 *
 * 当前阶段记录“你好小飞”唤醒结果。后续 AI 对话、音乐、灯光和网页配网
 * 业务通过此处把事件投递到业务队列，不能在语音回调中执行耗时操作。
 */
static void on_voice_service_event(
    const voice_service_event_t *event,
    void *user_context
)
{
    (void)user_context;
    if (event == NULL) {
        return;
    }

    switch (event->type) {
    case VOICE_SERVICE_EVENT_READY:
        ESP_LOGI(TAG, "语音服务初始化完成，唤醒词：你好小飞");
        break;
    case VOICE_SERVICE_EVENT_WAKE_DETECTED:
        ESP_LOGI(TAG, "业务层收到唤醒事件：你好小飞，概率：%.3f", event->probability);
        break;
    case VOICE_SERVICE_EVENT_AUDIO_ERROR:
    case VOICE_SERVICE_EVENT_MODEL_ERROR:
        ESP_LOGE(TAG, "语音服务错误：%s", esp_err_to_name(event->error));
        break;
    case VOICE_SERVICE_EVENT_TIMEOUT:
        ESP_LOGD(TAG, "一次语音识别窗口结束，继续监听你好小飞");
        break;
    case VOICE_SERVICE_EVENT_STOPPED:
        ESP_LOGI(TAG, "语音监听已停止");
        break;
    default:
        break;
    }
}

/**
 * @brief P4 应用入口。
 */
void app_main(void)
{
    initialize_nvs();

    esp_err_t error = p4_c5_wifi_start();
    if (error == ESP_OK) {
        ESP_LOGI(TAG, "P4/C5 Wi-Fi 配网组件已启动");
    } else {
        ESP_LOGE(TAG, "P4/C5 Wi-Fi 配网组件初始化失败：%s", esp_err_to_name(error));
    }

    ESP_ERROR_CHECK(voice_service_register_callback(on_voice_service_event, NULL));

    error = voice_service_init();
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "语音服务初始化失败：%s", esp_err_to_name(error));
        return;
    }

    error = voice_service_start();
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "语音监听启动失败：%s", esp_err_to_name(error));
        return;
    }

    ESP_LOGI(TAG, "系统启动完成，当前唤醒词为：你好小飞");
}
