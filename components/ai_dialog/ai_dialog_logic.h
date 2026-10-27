#pragma once

/**
 * @file ai_dialog_logic.h
 * @brief AI 对话逻辑层公共接口。
 *
 * 逻辑层只负责维护 AI 会话状态和处理抽象事件，不依赖 Coze 协议、
 * 音频编解码器或扬声器驱动。应用层收到硬件、后端和播放事件后，统一通过
 * 本接口驱动状态机，从而避免具体后端协议直接修改业务状态。
 */

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief AI 对话的高层状态。
 */
typedef enum {
    AI_DIALOG_LOGIC_STATE_IDLE = 0,       /**< 待机，等待本地唤醒词。 */
    AI_DIALOG_LOGIC_STATE_STARTING,       /**< 已唤醒，正在启动 Coze 直连会话。 */
    AI_DIALOG_LOGIC_STATE_LISTENING,      /**< 正在采集当前轮用户语音。 */
    AI_DIALOG_LOGIC_STATE_WAITING_REPLY,  /**< 当前轮已经提交，等待 AI 首段音频。 */
    AI_DIALOG_LOGIC_STATE_PLAYING         /**< 正在播放 AI 回复。 */
} ai_dialog_logic_state_t;

/**
 * @brief 逻辑层接收的事件。
 */
typedef enum {
    AI_DIALOG_LOGIC_EVENT_WAKE_DETECTED = 0, /**< 本地唤醒词已经检测到。 */
    AI_DIALOG_LOGIC_EVENT_BACKEND_READY,     /**< Coze 直连会话已经可以接收语音。 */
    AI_DIALOG_LOGIC_EVENT_TURN_COMMITTED,    /**< 当前轮语音已经提交给后端。 */
    AI_DIALOG_LOGIC_EVENT_AUDIO_STARTED,     /**< 后端开始返回回复音频。 */
    AI_DIALOG_LOGIC_EVENT_AUDIO_ENDED,       /**< 后端已经返回回复音频结束事件。 */
    AI_DIALOG_LOGIC_EVENT_PLAYBACK_DRAINED,  /**< 设备已经播放完当前回复。 */
    AI_DIALOG_LOGIC_EVENT_BACKEND_ERROR,     /**< 后端返回错误。 */
    AI_DIALOG_LOGIC_EVENT_BACKEND_STOPPED,   /**< 后端连接已经停止或断开。 */
    AI_DIALOG_LOGIC_EVENT_STOP               /**< 应用主动停止当前会话。 */
} ai_dialog_logic_event_t;

/**
 * @brief 初始化逻辑层。
 *
 * 逻辑层初始化后处于 AI_DIALOG_LOGIC_STATE_IDLE 状态。
 */
esp_err_t ai_dialog_logic_init(void);

/**
 * @brief 向逻辑层投递一个事件。
 *
 * 该函数只执行状态迁移，不执行网络、音频或硬件操作。应用层负责根据
 * 事件处理结果调用对应的后端和音频接口。
 */
esp_err_t ai_dialog_logic_handle_event(ai_dialog_logic_event_t event);

/**
 * @brief 强制清理当前会话并回到待机状态。
 *
 * 该接口用于异常和应用关闭场景，保证旧会话不会继续被认为有效。
 */
void ai_dialog_logic_reset(void);

/**
 * @brief 读取当前 AI 状态。
 */
ai_dialog_logic_state_t ai_dialog_logic_get_state(void);

/**
 * @brief 判断当前是否已经进入唤醒后的会话。
 */
bool ai_dialog_logic_is_session_active(void);

/**
 * @brief 判断当前是否允许应用层继续接收麦克风 PCM。
 */
bool ai_dialog_logic_can_capture(void);

#ifdef __cplusplus
}
#endif
