/**
 * @file device_state_machine.c
 * @brief 设备状态机组件实现 - 完整架构版本
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
static void action_network_connecting(void);
static void action_service_starting(void);
static void action_enter_idle(void);
static void action_ai_starting(void);
static void action_ai_listening(void);
static void action_ai_processing(void);
static void action_ai_playing(void);
static void action_ai_back_to_listening(void);
static void action_conversation_end(void);
static void action_voice_call_incoming(void);
static void action_voice_call_start(void);
static void action_voice_call_end(void);
static void action_video_call_incoming(void);
static void action_video_call_start(void);
static void action_video_call_end(void);
static void action_stop_ai_and_start_voice_call(void);
static void action_stop_ai_and_start_video_call(void);
static void action_error_recovery(void);

// 状态转换表
static const state_transition_t s_transition_table[] = {
    // 启动流程
    {DEVICE_STATE_BOOT, EVENT_WIFI_CONNECTED, DEVICE_STATE_NETWORK_CONNECTING, action_network_connecting},
    {DEVICE_STATE_NETWORK_CONNECTING, EVENT_NETWORK_READY, DEVICE_STATE_SERVICE_STARTING, action_service_starting},
    {DEVICE_STATE_SERVICE_STARTING, EVENT_SERVICE_READY, DEVICE_STATE_IDLE, action_enter_idle},

    // 待机状态转换
    {DEVICE_STATE_IDLE, EVENT_WAKE_WORD_DETECTED, DEVICE_STATE_AI_STARTING, action_ai_starting},
    {DEVICE_STATE_IDLE, EVENT_INCOMING_VOICE_CALL, DEVICE_STATE_VOICE_CALL_INCOMING, action_voice_call_incoming},
    {DEVICE_STATE_IDLE, EVENT_INCOMING_VIDEO_CALL, DEVICE_STATE_VIDEO_CALL_INCOMING, action_video_call_incoming},

    // AI对话状态转换
    {DEVICE_STATE_AI_STARTING, EVENT_AI_READY, DEVICE_STATE_AI_LISTENING, action_ai_listening},
    {DEVICE_STATE_AI_LISTENING, EVENT_USER_STOPPED, DEVICE_STATE_AI_PROCESSING, action_ai_processing},
    {DEVICE_STATE_AI_LISTENING, EVENT_CONVERSATION_ENDED, DEVICE_STATE_IDLE, action_conversation_end},
    {DEVICE_STATE_AI_LISTENING, EVENT_CONVERSATION_TIMEOUT, DEVICE_STATE_IDLE, action_conversation_end},
    {DEVICE_STATE_AI_PROCESSING, EVENT_AI_REPLY_START, DEVICE_STATE_AI_PLAYING, action_ai_playing},
    {DEVICE_STATE_AI_PLAYING, EVENT_AI_REPLY_END, DEVICE_STATE_AI_LISTENING, action_ai_back_to_listening},
    {DEVICE_STATE_AI_PLAYING, EVENT_CONVERSATION_ENDED, DEVICE_STATE_IDLE, action_conversation_end},

    // AI对话被来电打断
    {DEVICE_STATE_AI_STARTING, EVENT_INCOMING_VOICE_CALL, DEVICE_STATE_VOICE_CALL_INCOMING, action_stop_ai_and_start_voice_call},
    {DEVICE_STATE_AI_LISTENING, EVENT_INCOMING_VOICE_CALL, DEVICE_STATE_VOICE_CALL_INCOMING, action_stop_ai_and_start_voice_call},
    {DEVICE_STATE_AI_PROCESSING, EVENT_INCOMING_VOICE_CALL, DEVICE_STATE_VOICE_CALL_INCOMING, action_stop_ai_and_start_voice_call},
    {DEVICE_STATE_AI_PLAYING, EVENT_INCOMING_VOICE_CALL, DEVICE_STATE_VOICE_CALL_INCOMING, action_stop_ai_and_start_voice_call},
    {DEVICE_STATE_AI_STARTING, EVENT_INCOMING_VIDEO_CALL, DEVICE_STATE_VIDEO_CALL_INCOMING, action_stop_ai_and_start_video_call},
    {DEVICE_STATE_AI_LISTENING, EVENT_INCOMING_VIDEO_CALL, DEVICE_STATE_VIDEO_CALL_INCOMING, action_stop_ai_and_start_video_call},
    {DEVICE_STATE_AI_PROCESSING, EVENT_INCOMING_VIDEO_CALL, DEVICE_STATE_VIDEO_CALL_INCOMING, action_stop_ai_and_start_video_call},
    {DEVICE_STATE_AI_PLAYING, EVENT_INCOMING_VIDEO_CALL, DEVICE_STATE_VIDEO_CALL_INCOMING, action_stop_ai_and_start_video_call},

    // 语音通话状态转换
    {DEVICE_STATE_VOICE_CALL_INCOMING, EVENT_CALL_ACCEPTED, DEVICE_STATE_VOICE_CALL_ACTIVE, action_voice_call_start},
    {DEVICE_STATE_VOICE_CALL_INCOMING, EVENT_CALL_REJECTED, DEVICE_STATE_IDLE, action_voice_call_end},
    {DEVICE_STATE_VOICE_CALL_ACTIVE, EVENT_CALL_ENDED, DEVICE_STATE_IDLE, action_voice_call_end},
    {DEVICE_STATE_VOICE_CALL_ACTIVE, EVENT_USER_HANGUP, DEVICE_STATE_IDLE, action_voice_call_end},

    // 视频通话状态转换
    {DEVICE_STATE_VIDEO_CALL_INCOMING, EVENT_CALL_ACCEPTED, DEVICE_STATE_VIDEO_CALL_ACTIVE, action_video_call_start},
    {DEVICE_STATE_VIDEO_CALL_INCOMING, EVENT_CALL_REJECTED, DEVICE_STATE_IDLE, action_video_call_end},
    {DEVICE_STATE_VIDEO_CALL_ACTIVE, EVENT_CALL_ENDED, DEVICE_STATE_IDLE, action_video_call_end},
    {DEVICE_STATE_VIDEO_CALL_ACTIVE, EVENT_USER_HANGUP, DEVICE_STATE_IDLE, action_video_call_end},

    // 错误恢复
    {DEVICE_STATE_SERVICE_STARTING, EVENT_BACKEND_DISCONNECTED, DEVICE_STATE_ERROR_RECOVERY, action_error_recovery},
    {DEVICE_STATE_IDLE, EVENT_NETWORK_LOST, DEVICE_STATE_ERROR_RECOVERY, action_error_recovery},
    {DEVICE_STATE_AI_LISTENING, EVENT_COZE_ERROR, DEVICE_STATE_ERROR_RECOVERY, action_error_recovery},
    {DEVICE_STATE_AI_PROCESSING, EVENT_COZE_ERROR, DEVICE_STATE_ERROR_RECOVERY, action_error_recovery},
    {DEVICE_STATE_AI_PLAYING, EVENT_COZE_ERROR, DEVICE_STATE_ERROR_RECOVERY, action_error_recovery},
    {DEVICE_STATE_ERROR_RECOVERY, EVENT_RECOVERY_COMPLETE, DEVICE_STATE_SERVICE_STARTING, action_service_starting},
};

#define TRANSITION_TABLE_SIZE (sizeof(s_transition_table) / sizeof(state_transition_t))

// ========== 动作函数实现 ==========

static void action_network_connecting(void)
{
    ESP_LOGI(TAG, "Action: 网络连接中");
    if (s_ctx.callbacks.on_network_connecting) {
        s_ctx.callbacks.on_network_connecting();
    }
}

static void action_service_starting(void)
{
    ESP_LOGI(TAG, "Action: 启动后台服务");
    if (s_ctx.callbacks.on_service_starting) {
        s_ctx.callbacks.on_service_starting();
    }
}

static void action_enter_idle(void)
{
    ESP_LOGI(TAG, "Action: 进入待机状态");
    if (s_ctx.callbacks.on_enter_idle) {
        s_ctx.callbacks.on_enter_idle();
    }
}

static void action_ai_starting(void)
{
    ESP_LOGI(TAG, "Action: AI对话启动中");
    if (s_ctx.callbacks.on_ai_starting) {
        s_ctx.callbacks.on_ai_starting();
    }
}

static void action_ai_listening(void)
{
    ESP_LOGI(TAG, "Action: AI监听用户语音");
    if (s_ctx.callbacks.on_ai_listening) {
        s_ctx.callbacks.on_ai_listening();
    }
}

static void action_ai_processing(void)
{
    ESP_LOGI(TAG, "Action: AI处理中");
    if (s_ctx.callbacks.on_ai_processing) {
        s_ctx.callbacks.on_ai_processing();
    }
}

static void action_ai_playing(void)
{
    ESP_LOGI(TAG, "Action: AI播放回复");
    if (s_ctx.callbacks.on_ai_playing) {
        s_ctx.callbacks.on_ai_playing();
    }
}

static void action_ai_back_to_listening(void)
{
    ESP_LOGI(TAG, "Action: AI回复结束，继续监听");
    if (s_ctx.callbacks.on_ai_listening) {
        s_ctx.callbacks.on_ai_listening();
    }
}

static void action_conversation_end(void)
{
    ESP_LOGI(TAG, "Action: 对话结束，返回待机");
    if (s_ctx.callbacks.on_conversation_end) {
        s_ctx.callbacks.on_conversation_end();
    }
}

static void action_voice_call_incoming(void)
{
    ESP_LOGI(TAG, "Action: 语音来电振铃");
    if (s_ctx.callbacks.on_voice_call_incoming) {
        s_ctx.callbacks.on_voice_call_incoming();
    }
}

static void action_voice_call_start(void)
{
    ESP_LOGI(TAG, "Action: 接听语音通话");
    if (s_ctx.callbacks.on_voice_call_start) {
        s_ctx.callbacks.on_voice_call_start();
    }
}

static void action_voice_call_end(void)
{
    ESP_LOGI(TAG, "Action: 结束语音通话");
    if (s_ctx.callbacks.on_voice_call_end) {
        s_ctx.callbacks.on_voice_call_end();
    }
}

static void action_video_call_incoming(void)
{
    ESP_LOGI(TAG, "Action: 视频来电振铃");
    if (s_ctx.callbacks.on_video_call_incoming) {
        s_ctx.callbacks.on_video_call_incoming();
    }
}

static void action_video_call_start(void)
{
    ESP_LOGI(TAG, "Action: 接听视频通话");
    if (s_ctx.callbacks.on_video_call_start) {
        s_ctx.callbacks.on_video_call_start();
    }
}

static void action_video_call_end(void)
{
    ESP_LOGI(TAG, "Action: 结束视频通话");
    if (s_ctx.callbacks.on_video_call_end) {
        s_ctx.callbacks.on_video_call_end();
    }
}

static void action_stop_ai_and_start_voice_call(void)
{
    ESP_LOGI(TAG, "Action: 停止AI对话，接听语音来电");
    if (s_ctx.callbacks.on_conversation_end) {
        s_ctx.callbacks.on_conversation_end();
    }
    if (s_ctx.callbacks.on_voice_call_incoming) {
        s_ctx.callbacks.on_voice_call_incoming();
    }
}

static void action_stop_ai_and_start_video_call(void)
{
    ESP_LOGI(TAG, "Action: 停止AI对话，接听视频来电");
    if (s_ctx.callbacks.on_conversation_end) {
        s_ctx.callbacks.on_conversation_end();
    }
    if (s_ctx.callbacks.on_video_call_incoming) {
        s_ctx.callbacks.on_video_call_incoming();
    }
}

static void action_error_recovery(void)
{
    ESP_LOGI(TAG, "Action: 进入错误恢复");
    if (s_ctx.callbacks.on_error_recovery) {
        s_ctx.callbacks.on_error_recovery();
    }
}

// ========== 状态机核心逻辑 ==========

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

    // 更新状态（在执行动作之前更新，避免动作中查询状态看到旧状态）
    s_ctx.current_state = transition->next_state;

    xSemaphoreGive(s_ctx.state_mutex);

    // 执行转换动作（在锁外执行，避免阻塞状态机）
    if (transition->action) {
        transition->action();
    }

    // 通知状态变化
    if (s_ctx.callbacks.on_state_changed) {
        s_ctx.callbacks.on_state_changed(old_state, transition->next_state);
    }

    return true;
}

// 状态机任务
static void state_machine_task(void *arg)
{
    device_event_t event;

    ESP_LOGI(TAG, "状态机任务启动");

    while (s_ctx.is_running) {
        if (xQueueReceive(s_ctx.event_queue, &event, portMAX_DELAY) == pdTRUE) {
            if (event < EVENT_MAX) {
                execute_transition(event);
            }
        }
    }

    ESP_LOGI(TAG, "状态机任务退出");
    vTaskDelete(NULL);
}

// ========== 公共API实现 ==========

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

    // 创建事件队列（增大队列以支持更多事件）
    s_ctx.event_queue = xQueueCreate(20, sizeof(device_event_t));
    if (s_ctx.event_queue == NULL) {
        ESP_LOGE(TAG, "创建事件队列失败");
        vSemaphoreDelete(s_ctx.state_mutex);
        return false;
    }

    // 初始状态为启动状态
    s_ctx.current_state = DEVICE_STATE_BOOT;
    s_ctx.is_running = true;

    // 创建状态机任务
    BaseType_t ret = xTaskCreate(state_machine_task, "state_machine", 4096, NULL, 5, &s_ctx.task_handle);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "创建状态机任务失败");
        vQueueDelete(s_ctx.event_queue);
        vSemaphoreDelete(s_ctx.state_mutex);
        return false;
    }

    ESP_LOGI(TAG, "状态机初始化成功，初始状态: %s", device_state_get_name(s_ctx.current_state));
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
    xSemaphoreTake(s_ctx.state_mutex, portMAX_DELAY);
    device_state_t state = s_ctx.current_state;
    xSemaphoreGive(s_ctx.state_mutex);
    return state;
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
        case DEVICE_STATE_BOOT:
            return "BOOT";
        case DEVICE_STATE_NETWORK_CONNECTING:
            return "NETWORK_CONNECTING";
        case DEVICE_STATE_SERVICE_STARTING:
            return "SERVICE_STARTING";
        case DEVICE_STATE_IDLE:
            return "IDLE";
        case DEVICE_STATE_AI_STARTING:
            return "AI_STARTING";
        case DEVICE_STATE_AI_LISTENING:
            return "AI_LISTENING";
        case DEVICE_STATE_AI_PROCESSING:
            return "AI_PROCESSING";
        case DEVICE_STATE_AI_PLAYING:
            return "AI_PLAYING";
        case DEVICE_STATE_VOICE_CALL_INCOMING:
            return "VOICE_CALL_INCOMING";
        case DEVICE_STATE_VOICE_CALL_ACTIVE:
            return "VOICE_CALL_ACTIVE";
        case DEVICE_STATE_VIDEO_CALL_INCOMING:
            return "VIDEO_CALL_INCOMING";
        case DEVICE_STATE_VIDEO_CALL_ACTIVE:
            return "VIDEO_CALL_ACTIVE";
        case DEVICE_STATE_ERROR_RECOVERY:
            return "ERROR_RECOVERY";
        default:
            return "UNKNOWN";
    }
}

const char* device_event_get_name(device_event_t event)
{
    switch (event) {
        case EVENT_WIFI_CONNECTED:
            return "WIFI_CONNECTED";
        case EVENT_NETWORK_READY:
            return "NETWORK_READY";
        case EVENT_NETWORK_LOST:
            return "NETWORK_LOST";
        case EVENT_BACKEND_CONNECTED:
            return "BACKEND_CONNECTED";
        case EVENT_COZE_READY:
            return "COZE_READY";
        case EVENT_SERVICE_READY:
            return "SERVICE_READY";
        case EVENT_BACKEND_DISCONNECTED:
            return "BACKEND_DISCONNECTED";
        case EVENT_WAKE_WORD_DETECTED:
            return "WAKE_WORD_DETECTED";
        case EVENT_AI_READY:
            return "AI_READY";
        case EVENT_USER_SPEAKING:
            return "USER_SPEAKING";
        case EVENT_USER_STOPPED:
            return "USER_STOPPED";
        case EVENT_AI_REPLY_START:
            return "AI_REPLY_START";
        case EVENT_AI_REPLY_END:
            return "AI_REPLY_END";
        case EVENT_CONVERSATION_ENDED:
            return "CONVERSATION_ENDED";
        case EVENT_CONVERSATION_TIMEOUT:
            return "CONVERSATION_TIMEOUT";
        case EVENT_INCOMING_VOICE_CALL:
            return "INCOMING_VOICE_CALL";
        case EVENT_INCOMING_VIDEO_CALL:
            return "INCOMING_VIDEO_CALL";
        case EVENT_CALL_ACCEPTED:
            return "CALL_ACCEPTED";
        case EVENT_CALL_REJECTED:
            return "CALL_REJECTED";
        case EVENT_CALL_ENDED:
            return "CALL_ENDED";
        case EVENT_USER_HANGUP:
            return "USER_HANGUP";
        case EVENT_AUDIO_ERROR:
            return "AUDIO_ERROR";
        case EVENT_COZE_ERROR:
            return "COZE_ERROR";
        case EVENT_RECOVERY_COMPLETE:
            return "RECOVERY_COMPLETE";
        default:
            return "UNKNOWN";
    }
}
