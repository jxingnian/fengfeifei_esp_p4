#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief AI对话事件回调类型
 */
typedef enum {
    AI_DIALOG_EVENT_READY = 0,          // AI就绪，可以开始对话
    AI_DIALOG_EVENT_LISTENING,          // 开始监听用户
    AI_DIALOG_EVENT_USER_SPEAKING,      // 检测到用户说话
    AI_DIALOG_EVENT_USER_STOPPED,       // 用户停止说话
    AI_DIALOG_EVENT_AI_REPLY_START,     // AI开始回复
    AI_DIALOG_EVENT_AI_REPLY_END,       // AI回复结束
    AI_DIALOG_EVENT_SESSION_END,        // 会话结束
    AI_DIALOG_EVENT_ERROR,              // 错误
} ai_dialog_event_type_t;

typedef struct {
    ai_dialog_event_type_t type;
    const char *message;                // 错误消息（仅ERROR事件）
} ai_dialog_event_t;

typedef void (*ai_dialog_event_callback_t)(const ai_dialog_event_t *event, void *context);

typedef struct {
    ai_dialog_event_callback_t on_event;
    void *context;
} ai_dialog_config_t;

/**
 * @brief 初始化AI对话服务
 */
esp_err_t ai_dialog_init(const ai_dialog_config_t *config);

/**
 * @brief 反初始化AI对话服务
 */
void ai_dialog_deinit(void);

/**
 * @brief 开始AI对话会话（唤醒后调用）
 */
esp_err_t ai_dialog_start_session(void);

/**
 * @brief 结束AI对话会话
 */
esp_err_t ai_dialog_stop_session(const char *reason);

/**
 * @brief 接收麦克风PCM数据
 */
void ai_dialog_on_pcm(const int16_t *samples, size_t sample_count, void *context);

/**
 * @brief 查询当前是否处于会话中
 */
bool ai_dialog_is_session_active(void);

/**
 * @brief 处理来自 Coze 的音频数据
 * @param data Opus 编码的音频数据
 * @param length 数据长度
 * @param turn 对话轮次
 * @param sequence 音频序列号
 * @return ESP_OK 成功, ESP_ERR_INVALID_ARG 参数无效, ESP_FAIL 处理失败
 */
esp_err_t ai_dialog_on_audio_data(const uint8_t *data, size_t length, unsigned turn, unsigned sequence);

/**
 * @brief 通知AI对话层：AI开始播放回复
 * @note 由状态机在进入AI_PLAYING状态时调用，主动驱动内部状态切换
 */
void ai_dialog_on_reply_start(void);

/**
 * @brief 通知AI对话层：AI回复播放结束
 * @note 由状态机在AI回复结束时调用
 */
void ai_dialog_on_reply_end(void);

#ifdef __cplusplus
}
#endif
