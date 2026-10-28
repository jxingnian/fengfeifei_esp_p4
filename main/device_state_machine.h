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
    DEVICE_STATE_IDLE = 0,              // 待机状态
    DEVICE_STATE_AI_CONVERSATION,       // AI对话状态
    DEVICE_STATE_VOICE_CALL,            // 语音通话状态
    DEVICE_STATE_VIDEO_CALL,            // 视频通话状态
    DEVICE_STATE_MAX
} device_state_t;

/**
 * @brief 设备事件定义
 */
typedef enum {
    EVENT_WAKE_WORD_DETECTED = 0,       // 检测到唤醒词
    EVENT_INCOMING_VOICE_CALL,          // 语音来电
    EVENT_INCOMING_VIDEO_CALL,          // 视频来电
    EVENT_CALL_ENDED,                   // 通话结束
    EVENT_CONVERSATION_ENDED,           // 对话结束
    EVENT_CONVERSATION_TIMEOUT,         // 对话超时
    EVENT_USER_HANGUP,                  // 用户挂断
    EVENT_MAX
} device_event_t;

/**
 * @brief 状态机回调函数接口（由应用层实现）
 */
typedef struct {
    // AI对话相关回调
    void (*on_start_conversation)(void);     // 启动AI对话
    void (*on_stop_conversation)(void);      // 停止AI对话

    // 语音通话相关回调
    void (*on_start_voice_call)(void);       // 接听语音通话
    void (*on_stop_voice_call)(void);        // 结束语音通话

    // 视频通话相关回调
    void (*on_start_video_call)(void);       // 接听视频通话
    void (*on_stop_video_call)(void);        // 结束视频通话

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
