/*
 * @file app_main.c
 * @brief 当前阶段设备固件启动入口。
 *
 * 设备目前只启用 Wi-Fi 配网、统一音频服务、Coze 直连 AI 对话和本地
 * 唤醒词。视频通话、后台网关以及小程序链路不在本固件启动图中，后续
 * 接入新业务时应作为独立组件注册到系统状态机，而不是重新塞入本文件。
 */
#include "p4_c5_wifi.h"
#include "audio_service.h"
#include "ai_dialog.h"
#include "wake_word.h"

#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"

static const char *TAG = "app_main";

/**
 * @brief 初始化 NVS。
 *
 * Wi-Fi 配网组件和 ESP-Hosted 参数都依赖 NVS；遇到旧版本或空间不足时
 * 按 ESP-IDF 约定擦除后重新初始化，保证设备可以从干净状态启动。
 */
static void app_initialize_nvs(void)
{
    esp_err_t error = nvs_flash_init();
    if (error == ESP_ERR_NVS_NO_FREE_PAGES ||
        error == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        error = nvs_flash_init();
    }
    ESP_ERROR_CHECK(error);
}

/**
 * @brief 处理唤醒词服务事件。
 *
 * 回调只做状态日志和快速投递。AI 网络连接、编解码和播放全部在 ai_dialog
 * 自己的任务中执行，避免 Wi-Fi 或串口日志阻塞麦克风采集任务。
 */
static void app_on_wake_word_event(
    const wake_word_event_t *event,
    void *user_context
)
{
    (void)user_context;
    if (event == NULL) {
        return;
    }

    switch (event->type) {
    case WAKE_WORD_EVENT_READY:
        ESP_LOGI(TAG, "唤醒词服务就绪：你好小飞");
        break;
    case WAKE_WORD_EVENT_WAKE_DETECTED:
        ESP_LOGI(TAG, "收到唤醒词事件，概率：%.3f", event->probability);
        ai_dialog_on_wake_detected();
        break;
    case WAKE_WORD_EVENT_AUDIO_ERROR:
    case WAKE_WORD_EVENT_MODEL_ERROR:
        ESP_LOGE(TAG, "唤醒词服务错误：%s", esp_err_to_name(event->error));
        break;
    case WAKE_WORD_EVENT_TIMEOUT:
        ESP_LOGD(TAG, "本次唤醒识别窗口结束，继续监听");
        break;
    case WAKE_WORD_EVENT_STOPPED:
        ESP_LOGI(TAG, "唤醒词采集任务已停止");
        break;
    default:
        break;
    }
}

/**
 * @brief 把统一音频服务输出的麦克风 PCM 投递给 AI 对话层。
 *
 * 该回调运行在唤醒词采集任务中，只允许非阻塞投递；音频编码和网络发送
 * 由 ai_dialog 的独立任务完成。
 */
static void app_on_capture_pcm(
    const int16_t *samples,
    size_t sample_count,
    void *user_context
)
{
    (void)user_context;
    ai_dialog_on_pcm(samples, sample_count, NULL);
}

/**
 * @brief P4 主应用入口。
 */
void app_main(void)
{
    app_initialize_nvs();

    esp_err_t error = p4_c5_wifi_start();
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi 配网服务启动失败：%s", esp_err_to_name(error));
        return;
    }
    ESP_LOGI(TAG, "Wi-Fi 配网服务已启动");

    /* 统一音频服务必须早于唤醒词和 AI 对话初始化，确保两者共享同一套
     * ES8311/I2S 资源，并让音量配置在启动阶段一次性写入硬件。 */
    ESP_ERROR_CHECK(audio_service_init());
    ESP_ERROR_CHECK(wake_word_register_callback(app_on_wake_word_event, NULL));
    ESP_ERROR_CHECK(wake_word_register_pcm_callback(app_on_capture_pcm, NULL));
    ESP_ERROR_CHECK(ai_dialog_init());

    error = wake_word_init();
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "唤醒词服务初始化失败：%s", esp_err_to_name(error));
        return;
    }

    error = wake_word_start();
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "唤醒词监听启动失败：%s", esp_err_to_name(error));
        return;
    }

    ESP_LOGI(TAG, "系统启动完成：待机监听 -> Coze 直连 AI 对话");
}
