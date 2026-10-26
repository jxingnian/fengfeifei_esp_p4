/*
 * @file voice_service.h
 * @brief ESP32-P4 语音唤醒服务公共接口。
 *
 * 本组件使用开发板上的 ES8311 模拟麦克风输入，并通过 ESP-SR 中文
 * MultiNet 识别“你好小飞”。组件向业务层只输出事件，不暴露音频设备、
 * MultiNet 模型和底层缓冲区，避免业务代码与算法实现耦合。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 语音服务状态。
 */
typedef enum {
    VOICE_SERVICE_STATE_STOPPED = 0, /**< 服务未启动。 */
    VOICE_SERVICE_STATE_STARTING,    /**< 正在初始化音频和模型。 */
    VOICE_SERVICE_STATE_LISTENING,   /**< 正在持续监听“你好小飞”。 */
    VOICE_SERVICE_STATE_ERROR,       /**< 服务发生错误，已停止识别。 */
} voice_service_state_t;

/**
 * @brief 语音服务事件类型。
 */
typedef enum {
    VOICE_SERVICE_EVENT_READY = 0,     /**< 音频和模型初始化成功。 */
    VOICE_SERVICE_EVENT_WAKE_DETECTED, /**< 识别到“你好小飞”。 */
    VOICE_SERVICE_EVENT_TIMEOUT,       /**< 一次 MultiNet 识别窗口结束。 */
    VOICE_SERVICE_EVENT_AUDIO_ERROR,   /**< 麦克风读取失败。 */
    VOICE_SERVICE_EVENT_MODEL_ERROR,   /**< MultiNet 模型或命令表初始化失败。 */
    VOICE_SERVICE_EVENT_STOPPED,       /**< 服务停止。 */
} voice_service_event_type_t;

/**
 * @brief 语音服务事件数据。
 */
typedef struct {
    voice_service_event_type_t type; /**< 事件类型。 */
    voice_service_state_t state;     /**< 事件发生时的服务状态。 */
    int command_id;                  /**< MultiNet 命令 ID，唤醒词固定为 1。 */
    float probability;               /**< MultiNet 返回的识别概率。 */
    esp_err_t error;                 /**< 关联错误码，正常事件为 ESP_OK。 */
} voice_service_event_t;

/**
 * @brief 业务层事件回调。
 *
 * 回调运行在语音任务中，只允许快速处理或投递业务队列，不能在回调中
 * 执行联网、播放文件等长时间阻塞操作。
 */
typedef void (*voice_service_event_callback_t)(
    const voice_service_event_t *event,
    void *user_context
);

/**
 * @brief 统一麦克风采集任务输出的 PCM 回调。
 *
 * 回调运行在 voice_service 的 FreeRTOS 任务中，调用方只能执行快速、
 * 非阻塞的数据投递，不得在回调中等待网络、播放音频或执行长时间计算。
 */
typedef void (*voice_service_pcm_callback_t)(
    const int16_t *samples,
    size_t sample_count,
    void *user_context
);

/**
 * @brief 注册业务层事件回调。
 */
esp_err_t voice_service_register_callback(
    voice_service_event_callback_t callback,
    void *user_context
);

/**
 * @brief 注册统一麦克风 PCM 输出回调。
 */
esp_err_t voice_service_register_pcm_callback(
    voice_service_pcm_callback_t callback,
    void *user_context
);

/**
 * @brief 初始化 ES8311 麦克风、模型和“你好小飞”命令词。
 */
esp_err_t voice_service_init(void);

/**
 * @brief 启动持续识别任务。
 */
esp_err_t voice_service_start(void);

/**
 * @brief 停止持续识别任务。
 */
esp_err_t voice_service_stop(void);

/**
 * @brief 将当前语音识别任务切换到唤醒后的连续对话采集模式。
 *
 * 该接口只改变采集任务的工作模式，不创建新的麦克风读取任务，
 * 从根源上避免唤醒识别和 AI 上传同时读取 ES8311。
 */
void voice_service_start_conversation(void);

/**
 * @brief 退出连续对话采集模式并恢复唤醒词监听模式。
 */
void voice_service_stop_conversation(void);

/**
 * @brief 查询当前是否处于连续对话采集模式。
 */
bool voice_service_is_conversation_active(void);

/**
 * @brief 查询当前语音服务状态。
 */
voice_service_state_t voice_service_get_state(void);

#ifdef __cplusplus
}
#endif
