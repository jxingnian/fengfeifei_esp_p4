/**
 * @file device_state_machine_example.c
 * @brief 设备状态机使用示例
 *
 * 展示如何在应用层使用状态机组件
 */

#include "device_state_machine.h"
#include "esp_log.h"

static const char *TAG = "StateMachineExample";

// ========== 应用层回调函数实现 ==========

static void on_start_conversation_callback(void)
{
    ESP_LOGI(TAG, "[应用层] 开始AI对话");
    // TODO: 应用层实现具体逻辑：
    // 1. 初始化语音识别
    // 2. 启动TTS
    // 3. 连接AI服务
    // 4. 开始录音
}

static void on_stop_conversation_callback(void)
{
    ESP_LOGI(TAG, "[应用层] 停止AI对话");
    // TODO: 应用层实现具体逻辑：
    // 1. 立即停止语音识别
    // 2. 停止TTS播放
    // 3. 取消正在进行的AI请求
    // 4. 释放音频资源
    // 注意：这个回调必须快速返回，不能阻塞！
}

static void on_start_voice_call_callback(void)
{
    ESP_LOGI(TAG, "[应用层] 接听语音通话");
    // TODO: 应用层实现具体逻辑：
    // 1. 接听电话
    // 2. 初始化音频通道
    // 3. 开始语音流传输
}

static void on_stop_voice_call_callback(void)
{
    ESP_LOGI(TAG, "[应用层] 结束语音通话");
    // TODO: 应用层实现具体逻辑：
    // 1. 挂断电话
    // 2. 释放音频通道
    // 3. 清理通话资源
}

static void on_start_video_call_callback(void)
{
    ESP_LOGI(TAG, "[应用层] 接听视频通话");
    // TODO: 应用层实现具体逻辑：
    // 1. 接听视频电话
    // 2. 初始化音视频通道
    // 3. 开始音视频流传输
}

static void on_stop_video_call_callback(void)
{
    ESP_LOGI(TAG, "[应用层] 结束视频通话");
    // TODO: 应用层实现具体逻辑：
    // 1. 挂断视频电话
    // 2. 释放音视频通道
    // 3. 清理通话资源
}

static void on_state_changed_callback(device_state_t old_state, device_state_t new_state)
{
    ESP_LOGI(TAG, "[应用层] 状态变化: %s -> %s",
             device_state_get_name(old_state),
             device_state_get_name(new_state));

    // TODO: 应用层可以在这里做一些状态变化的通用处理：
    // 1. 更新UI显示
    // 2. 记录状态日志
    // 3. 上报状态到服务器
    // 4. 控制LED指示灯
}

// ========== 初始化和使用示例 ==========

void device_state_machine_example_init(void)
{
    // 1. 准备回调函数结构体
    device_state_callbacks_t callbacks = {
        .on_start_conversation = on_start_conversation_callback,
        .on_stop_conversation = on_stop_conversation_callback,
        .on_start_voice_call = on_start_voice_call_callback,
        .on_stop_voice_call = on_stop_voice_call_callback,
        .on_start_video_call = on_start_video_call_callback,
        .on_stop_video_call = on_stop_video_call_callback,
        .on_state_changed = on_state_changed_callback,
    };

    // 2. 初始化状态机
    if (device_state_machine_init(&callbacks)) {
        ESP_LOGI(TAG, "状态机初始化成功");
    } else {
        ESP_LOGE(TAG, "状态机初始化失败");
    }
}

// ========== 使用场景示例 ==========

// 场景1：用户说出唤醒词
void example_wake_word_detected(void)
{
    ESP_LOGI(TAG, "检测到唤醒词，发送事件到状态机");
    device_state_send_event(EVENT_WAKE_WORD_DETECTED);
}

// 场景2：AI对话结束
void example_conversation_ended(void)
{
    ESP_LOGI(TAG, "AI对话结束，发送事件到状态机");
    device_state_send_event(EVENT_CONVERSATION_ENDED);
}

// 场景3：收到语音来电
void example_incoming_voice_call(void)
{
    ESP_LOGI(TAG, "收到语音来电，发送事件到状态机");
    device_state_send_event(EVENT_INCOMING_VOICE_CALL);
}

// 场景4：收到视频来电
void example_incoming_video_call(void)
{
    ESP_LOGI(TAG, "收到视频来电，发送事件到状态机");
    device_state_send_event(EVENT_INCOMING_VIDEO_CALL);
}

// 场景5：通话结束
void example_call_ended(void)
{
    ESP_LOGI(TAG, "通话结束，发送事件到状态机");
    device_state_send_event(EVENT_CALL_ENDED);
}

// 场景6：用户主动挂断
void example_user_hangup(void)
{
    ESP_LOGI(TAG, "用户挂断，发送事件到状态机");
    device_state_send_event(EVENT_USER_HANGUP);
}

// 场景7：对话超时
void example_conversation_timeout(void)
{
    ESP_LOGI(TAG, "对话超时，发送事件到状态机");
    device_state_send_event(EVENT_CONVERSATION_TIMEOUT);
}

// 场景8：查询当前状态
void example_query_current_state(void)
{
    device_state_t current = device_state_get_current();
    ESP_LOGI(TAG, "当前状态: %s", device_state_get_name(current));
}

// 场景9：检查能否转换到某个状态
void example_check_can_transit(void)
{
    if (device_state_can_transit(DEVICE_STATE_VOICE_CALL)) {
        ESP_LOGI(TAG, "可以转换到语音通话状态");
    } else {
        ESP_LOGI(TAG, "不能转换到语音通话状态");
    }
}

// ========== 完整流程示例 ==========

/**
 * 示例：待机 -> AI对话 -> 来电 -> 通话 -> 待机
 */
void example_complete_flow(void)
{
    ESP_LOGI(TAG, "=== 开始完整流程测试 ===");

    // 1. 待机状态，检测到唤醒词
    vTaskDelay(pdMS_TO_TICKS(2000));
    example_wake_word_detected();

    // 2. 进入AI对话状态，对话中...
    vTaskDelay(pdMS_TO_TICKS(5000));
    example_query_current_state();

    // 3. 对话中收到来电（状态机会自动停止对话并接听电话）
    vTaskDelay(pdMS_TO_TICKS(3000));
    example_incoming_voice_call();

    // 4. 通话中...
    vTaskDelay(pdMS_TO_TICKS(10000));
    example_query_current_state();

    // 5. 通话结束，返回待机
    vTaskDelay(pdMS_TO_TICKS(2000));
    example_call_ended();

    vTaskDelay(pdMS_TO_TICKS(1000));
    example_query_current_state();

    ESP_LOGI(TAG, "=== 完整流程测试结束 ===");
}
