/**
 * @file device_state_machine.h
 * @brief 设备状态机组件 - 管理设备状态转换
 *
 * 负责管理设备的待机、AI对话、语音通话、视频通话等状态
 * 不负责具体业务实现，只负责状态切换和通知应用层
 */

#ifndef DEVICE_STATE_MACHINE_H
#define DEVICE_STATE_MACHINE_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 设备状态定义
 */
typedef enum {
    DEVICE_STATE_BOOT = 0,              // 开机初始化
    DEVICE_STATE_NETWORK_CONNECTING,    // 网络连接中
    DEVICE_STATE_SERVICE_STARTING,      // 后台服务启动中
    DEVICE_STATE_IDLE,                  // 待机状态（唤醒词监听）
    DEVICE_STATE_AI_STARTING,           // AI对话启动中
    DEVICE_STATE_AI_LISTENING,          // AI对话监听中
    DEVICE_STATE_AI_PROCESSING,         // AI处理中（等待回复）
    DEVICE_STATE_AI_PLAYING,            // AI回复播放中
    DEVICE_STATE_VOICE_CALL_INCOMING,   // 语音来电振铃
    DEVICE_STATE_VOICE_CALL_ACTIVE,     // 语音通话中
    DEVICE_STATE_VIDEO_CALL_INCOMING,   // 视频来电振铃
    DEVICE_STATE_VIDEO_CALL_ACTIVE,     // 视频通话中
    DEVICE_STATE_ERROR_RECOVERY,        // 错误恢复中
    DEVICE_STATE_MAX
} device_state_t;

/**
 * @brief 设备事件定义
 */
typedef enum {
    // 网络事件
    EVENT_WIFI_CONNECTED = 0,           // Wi-Fi连接成功
    EVENT_NETWORK_READY,                // 网络就绪（获取IP）
    EVENT_NETWORK_LOST,                 // 网络断开

    // 服务事件
    EVENT_BACKEND_CONNECTED,            // 后台连接成功
    EVENT_COZE_READY,                   // Coze服务就绪
    EVENT_SERVICE_READY,                // 所有服务就绪
    EVENT_BACKEND_DISCONNECTED,         // 后台断开

    // AI对话事件
    EVENT_WAKE_WORD_DETECTED,           // 检测到唤醒词
    EVENT_AI_READY,                     // AI服务就绪，可以开始对话
    EVENT_USER_SPEAKING,                // 用户开始说话
    EVENT_USER_STOPPED,                 // 用户停止说话
    EVENT_AI_REPLY_START,               // AI开始回复
    EVENT_AI_REPLY_END,                 // AI回复结束
    EVENT_CONVERSATION_ENDED,           // 对话结束
    EVENT_CONVERSATION_TIMEOUT,         // 对话超时

    // 通话事件
    EVENT_INCOMING_VOICE_CALL,          // 语音来电
    EVENT_INCOMING_VIDEO_CALL,          // 视频来电
    EVENT_CALL_ACCEPTED,                // 接听电话
    EVENT_CALL_REJECTED,                // 拒接电话
    EVENT_CALL_ENDED,                   // 通话结束
    EVENT_USER_HANGUP,                  // 用户挂断

    // 错误事件
    EVENT_AUDIO_ERROR,                  // 音频错误
    EVENT_COZE_ERROR,                   // Coze服务错误
    EVENT_RECOVERY_COMPLETE,            // 恢复完成

    EVENT_MAX
} device_event_t;

/**
 * @brief 状态机回调函数接口（由应用层实现）
 */
typedef struct {
    // 网络和服务回调
    void (*on_network_connecting)(void);     // 开始连接网络
    void (*on_service_starting)(void);       // 启动后台服务
    void (*on_enter_idle)(void);             // 进入待机状态

    // AI对话相关回调
    void (*on_ai_starting)(void);            // AI对话启动中
    void (*on_ai_listening)(void);           // 进入AI监听状态
    void (*on_ai_processing)(void);          // AI处理中
    void (*on_ai_playing)(void);             // AI播放中
    void (*on_conversation_end)(void);       // 对话结束，返回待机

    // 语音通话相关回调
    void (*on_voice_call_incoming)(void);    // 语音来电振铃
    void (*on_voice_call_start)(void);       // 接听语音通话
    void (*on_voice_call_end)(void);         // 结束语音通话

    // 视频通话相关回调
    void (*on_video_call_incoming)(void);    // 视频来电振铃
    void (*on_video_call_start)(void);       // 接听视频通话
    void (*on_video_call_end)(void);         // 结束视频通话

    // 错误恢复回调
    void (*on_error_recovery)(void);         // 进入错误恢复

    // 状态变化通知回调
    void (*on_state_changed)(device_state_t old_state, device_state_t new_state);
} device_state_callbacks_t;

/**
 * @brief 初始化设备状态机
 *
 * @param callbacks 应用层提供的回调函数，不能为NULL
 * @return true 初始化成功
 * @return false 初始化失败
 */
bool device_state_machine_init(const device_state_callbacks_t *callbacks);

/**
 * @brief 反初始化状态机，释放资源
 */
void device_state_machine_deinit(void);

/**
 * @brief 发送事件到状态机（触发状态转换）
 *
 * @param event 要发送的事件
 * @return true 事件处理成功
 * @return false 事件处理失败或被拒绝
 */
bool device_state_send_event(device_event_t event);

/**
 * @brief 获取当前设备状态
 *
 * @return device_state_t 当前状态
 */
device_state_t device_state_get_current(void);

/**
 * @brief 检查是否可以转换到目标状态
 *
 * @param target_state 目标状态
 * @return true 可以转换
 * @return false 不能转换
 */
bool device_state_can_transit(device_state_t target_state);

/**
 * @brief 获取状态名称字符串（用于调试）
 *
 * @param state 状态
 * @return const char* 状态名称
 */
const char* device_state_get_name(device_state_t state);

/**
 * @brief 获取事件名称字符串（用于调试）
 *
 * @param event 事件
 * @return const char* 事件名称
 */
const char* device_event_get_name(device_event_t event);

#ifdef __cplusplus
}
#endif

#endif // DEVICE_STATE_MACHINE_H
