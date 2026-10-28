/**
 * @file device_state_machine.c
 * @brief 设备状态机组件实现
 */

#include "device_state_machine.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "DeviceStateMachine";

// 状态机上下文
typedef struct {
    device_state_t current_state;           // 当前状态
    device_state_callbacks_t callbacks;     // 回调函数
    SemaphoreHandle_t state_mutex;          // 状态互斥锁
    QueueHandle_t event_queue;              // 事件队列
    TaskHandle_t task_handle;               // 状态机任务句柄
    bool is_running;                        // 运行标志
} state_machine_context_t;

static state_machine_context_t s_ctx = {0};

// 状态转换表结构
typedef struct {
    device_state_t current_state;           // 当前状态
    device_event_t event;                   // 触发事件
    device_state_t next_state;              // 下一个状态
    void (*action)(void);                   // 转换时执行的动作
} state_transition_t;

// 前置声明动作函数
static void action_start_conversation(void);
static void action_stop_conversation_and_start_voice_call(void);
static void action_stop_conversation_and_start_video_call(void);
static void action_stop_conversation(void);
static void action_start_voice_call(void);
static void action_stop_voice_call(void);
static void action_start_video_call(void);
static void action_stop_video_call(void);

// 状态转换表
static const state_transition_t s_transition_table[] = {
    // 从待机状态的转换
    {DEVICE_STATE_IDLE, EVENT_WAKE_WORD_DETECTED, DEVICE_STATE_AI_CONVERSATION, action_start_conversation},
    {DEVICE_STATE_IDLE, EVENT_INCOMING_VOICE_CALL, DEVICE_STATE_VOICE_CALL, action_start_voice_call},
    {DEVICE_STATE_IDLE, EVENT_INCOMING_VIDEO_CALL, DEVICE_STATE_VIDEO_CALL, action_start_video_call},

    // 从AI对话状态的转换
    {DEVICE_STATE_AI_CONVERSATION, EVENT_CONVERSATION_ENDED, DEVICE_STATE_IDLE, action_stop_conversation},
    {DEVICE_STATE_AI_CONVERSATION, EVENT_CONVERSATION_TIMEOUT, DEVICE_STATE_IDLE, action_stop_conversation},
    {DEVICE_STATE_AI_CONVERSATION, EVENT_INCOMING_VOICE_CALL, DEVICE_STATE_VOICE_CALL, action_stop_conversation_and_start_voice_call},
    {DEVICE_STATE_AI_CONVERSATION, EVENT_INCOMING_VIDEO_CALL, DEVICE_STATE_VIDEO_CALL, action_stop_conversation_and_start_video_call},

    // 从语音通话状态的转换
    {DEVICE_STATE_VOICE_CALL, EVENT_CALL_ENDED, DEVICE_STATE_IDLE, action_stop_voice_call},
    {DEVICE_STATE_VOICE_CALL, EVENT_USER_HANGUP, DEVICE_STATE_IDLE, action_stop_voice_call},

    // 从视频通话状态的转换
    {DEVICE_STATE_VIDEO_CALL, EVENT_CALL_ENDED, DEVICE_STATE_IDLE, action_stop_video_call},
    {DEVICE_STATE_VIDEO_CALL, EVENT_USER_HANGUP, DEVICE_STATE_IDLE, action_stop_video_call},
};

#define TRANSITION_TABLE_SIZE (sizeof(s_transition_table) / sizeof(state_transition_t))

// 动作函数实现
static void action_start_conversation(void)
{
    ESP_LOGI(TAG, "Action: 启动AI对话");
    if (s_ctx.callbacks.on_start_conversation) {
        s_ctx.callbacks.on_start_conversation();
    }
}

static void action_stop_conversation(void)
{
    ESP_LOGI(TAG, "Action: 停止AI对话");
    if (s_ctx.callbacks.on_stop_conversation) {
        s_ctx.callbacks.on_stop_conversation();
    }
}

static void action_start_voice_call(void)
{
    ESP_LOGI(TAG, "Action: 接听语音通话");
    if (s_ctx.callbacks.on_start_voice_call) {
        s_ctx.callbacks.on_start_voice_call();
    }
}

static void action_stop_voice_call(void)
{
    ESP_LOGI(TAG, "Action: 结束语音通话");
    if (s_ctx.callbacks.on_stop_voice_call) {
        s_ctx.callbacks.on_stop_voice_call();
    }
}

static void action_start_video_call(void)
{
    ESP_LOGI(TAG, "Action: 接听视频通话");
    if (s_ctx.callbacks.on_start_video_call) {
        s_ctx.callbacks.on_start_video_call();
    }
}

static void action_stop_video_call(void)
{
    ESP_LOGI(TAG, "Action: 结束视频通话");
    if (s_ctx.callbacks.on_stop_video_call) {
        s_ctx.callbacks.on_stop_video_call();
    }
}

static void action_stop_conversation_and_start_voice_call(void)
{
    ESP_LOGI(TAG, "Action: 停止AI对话并接听语音通话");
    if (s_ctx.callbacks.on_stop_conversation) {
        s_ctx.callbacks.on_stop_conversation();
    }
    if (s_ctx.callbacks.on_start_voice_call) {
        s_ctx.callbacks.on_start_voice_call();
    }
}

static void action_stop_conversation_and_start_video_call(void)
{
    ESP_LOGI(TAG, "Action: 停止AI对话并接听视频通话");
    if (s_ctx.callbacks.on_stop_conversation) {
        s_ctx.callbacks.on_stop_conversation();
    }
    if (s_ctx.callbacks.on_start_video_call) {
        s_ctx.callbacks.on_start_video_call();
    }
}

// 查找状态转换
static const state_transition_t* find_transition(device_state_t current_state, device_event_t event)
{
    for (int i = 0; i < TRANSITION_TABLE_SIZE; i++) {
        if (s_transition_table[i].current_state == current_state &&
            s_transition_table[i].event == event) {
            return &s_transition_table[i];
        }
    }
    return NULL;
}

// 执行状态转换
static bool execute_transition(device_event_t event)
{
    xSemaphoreTake(s_ctx.state_mutex, portMAX_DELAY);

    device_state_t old_state = s_ctx.current_state;
    const state_transition_t *transition = find_transition(old_state, event);

    if (transition == NULL) {
        ESP_LOGW(TAG, "无效的状态转换: state=%s, event=%s",
                 device_state_get_name(old_state),
                 device_event_get_name(event));
        xSemaphoreGive(s_ctx.state_mutex);
        return false;
    }

    ESP_LOGI(TAG, "状态转换: %s -> %s (event: %s)",
             device_state_get_name(old_state),
             device_state_get_name(transition->next_state),
             device_event_get_name(event));

    // 执行转换动作
    if (transition->action) {
        transition->action();
    }

    // 更新状态
    s_ctx.current_state = transition->next_state;

    // 通知状态变化
    if (s_ctx.callbacks.on_state_changed) {
        s_ctx.callbacks.on_state_changed(old_state, transition->next_state);
    }

    xSemaphoreGive(s_ctx.state_mutex);
    return true;
}

// 状态机任务
static void state_machine_task(void *arg)
{
    device_event_t event;

    ESP_LOGI(TAG, "状态机任务启动");

    while (s_ctx.is_running) {
        if (xQueueReceive(s_ctx.event_queue, &event, portMAX_DELAY) == pdTRUE) {
            execute_transition(event);
        }
    }

    ESP_LOGI(TAG, "状态机任务退出");
    vTaskDelete(NULL);
}

// 公共API实现
bool device_state_machine_init(const device_state_callbacks_t *callbacks)
{
    if (callbacks == NULL) {
        ESP_LOGE(TAG, "回调函数不能为NULL");
        return false;
    }

    if (s_ctx.is_running) {
        ESP_LOGW(TAG, "状态机已经初始化");
        return false;
    }

    memset(&s_ctx, 0, sizeof(state_machine_context_t));
    memcpy(&s_ctx.callbacks, callbacks, sizeof(device_state_callbacks_t));

    // 创建互斥锁
    s_ctx.state_mutex = xSemaphoreCreateMutex();
    if (s_ctx.state_mutex == NULL) {
        ESP_LOGE(TAG, "创建互斥锁失败");
        return false;
    }

    // 创建事件队列
    s_ctx.event_queue = xQueueCreate(10, sizeof(device_event_t));
    if (s_ctx.event_queue == NULL) {
        ESP_LOGE(TAG, "创建事件队列失败");
        vSemaphoreDelete(s_ctx.state_mutex);
        return false;
    }

    // 初始状态为待机
    s_ctx.current_state = DEVICE_STATE_IDLE;
    s_ctx.is_running = true;

    // 创建状态机任务
    BaseType_t ret = xTaskCreate(state_machine_task, "state_machine", 4096, NULL, 5, &s_ctx.task_handle);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "创建状态机任务失败");
        vQueueDelete(s_ctx.event_queue);
        vSemaphoreDelete(s_ctx.state_mutex);
        return false;
    }

    ESP_LOGI(TAG, "状态机初始化成功");
    return true;
}

void device_state_machine_deinit(void)
{
    if (!s_ctx.is_running) {
        return;
    }

    s_ctx.is_running = false;

    // 发送一个空事件唤醒任务
    device_event_t dummy_event = EVENT_MAX;
    xQueueSend(s_ctx.event_queue, &dummy_event, 0);

    vTaskDelay(pdMS_TO_TICKS(100));

    if (s_ctx.event_queue) {
        vQueueDelete(s_ctx.event_queue);
    }

    if (s_ctx.state_mutex) {
        vSemaphoreDelete(s_ctx.state_mutex);
    }

    memset(&s_ctx, 0, sizeof(state_machine_context_t));
    ESP_LOGI(TAG, "状态机已反初始化");
}

bool device_state_send_event(device_event_t event)
{
    if (!s_ctx.is_running) {
        ESP_LOGE(TAG, "状态机未初始化");
        return false;
    }

    if (event >= EVENT_MAX) {
        ESP_LOGE(TAG, "无效的事件: %d", event);
        return false;
    }

    if (xQueueSend(s_ctx.event_queue, &event, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "事件队列已满，丢弃事件: %s", device_event_get_name(event));
        return false;
    }

    return true;
}

device_state_t device_state_get_current(void)
{
    return s_ctx.current_state;
}

bool device_state_can_transit(device_state_t target_state)
{
    if (target_state >= DEVICE_STATE_MAX) {
        return false;
    }

    xSemaphoreTake(s_ctx.state_mutex, portMAX_DELAY);
    device_state_t current = s_ctx.current_state;
    xSemaphoreGive(s_ctx.state_mutex);

    // 遍历转换表，查找是否存在到目标状态的转换
    for (int i = 0; i < TRANSITION_TABLE_SIZE; i++) {
        if (s_transition_table[i].current_state == current &&
            s_transition_table[i].next_state == target_state) {
            return true;
        }
    }

    return false;
}

const char* device_state_get_name(device_state_t state)
{
    switch (state) {
        case DEVICE_STATE_IDLE:
            return "IDLE";
        case DEVICE_STATE_AI_CONVERSATION:
            return "AI_CONVERSATION";
        case DEVICE_STATE_VOICE_CALL:
            return "VOICE_CALL";
        case DEVICE_STATE_VIDEO_CALL:
            return "VIDEO_CALL";
        default:
            return "UNKNOWN";
    }
}

const char* device_event_get_name(device_event_t event)
{
    switch (event) {
        case EVENT_WAKE_WORD_DETECTED:
            return "WAKE_WORD_DETECTED";
        case EVENT_INCOMING_VOICE_CALL:
            return "INCOMING_VOICE_CALL";
        case EVENT_INCOMING_VIDEO_CALL:
            return "INCOMING_VIDEO_CALL";
        case EVENT_CALL_ENDED:
            return "CALL_ENDED";
        case EVENT_CONVERSATION_ENDED:
            return "CONVERSATION_ENDED";
        case EVENT_CONVERSATION_TIMEOUT:
            return "CONVERSATION_TIMEOUT";
        case EVENT_USER_HANGUP:
            return "USER_HANGUP";
        default:
            return "UNKNOWN";
    }
}
