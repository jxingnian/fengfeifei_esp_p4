/*
 * @file app_main.c
 * @brief 设备固件主入口 - 完整架构版本
 *
 * 本版本采用分层架构设计：
 * - 设备状态机：统一调度、资源仲裁、状态转换
 * - 应用层：AI对话、语音通话、视频通话业务
 * - 服务层：音频服务、Coze服务、后台连接服务
 * - 驱动层：音频驱动、网络驱动、视频驱动
 */

#include "esp_err.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// 驱动和服务层
#include "p4_c5_wifi.h"
#include "audio_service.h"

// 业务组件
#include "wake_word.h"
#include "ai_dialog.h"
#include "coze_app.h"
#include "voice_call.h"
#include "video_call.h"

// 设备状态机
#include "device_state_machine.h"

static const char *TAG = "app_main";

// ========== 全局变量 ==========
static bool s_coze_connected = false;
static unsigned s_ai_turn = 0;

// ========== NVS初始化 ==========

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

// ========== Coze事件处理 ==========

static void app_on_coze_event(const coze_app_event_t *event, void *context)
{
    (void)context;
    if (event == NULL) {
        return;
    }

    switch (event->type) {
    case COZE_APP_EVENT_CONNECTED:
        ESP_LOGI(TAG, "Coze WebSocket已连接");
        s_coze_connected = true;
        break;

    case COZE_APP_EVENT_READY:
        ESP_LOGI(TAG, "Coze服务就绪");
        device_state_send_event(EVENT_SERVICE_READY);
        break;

    case COZE_APP_EVENT_AUDIO_START:
        ESP_LOGI(TAG, "AI开始回复");
        device_state_send_event(EVENT_AI_REPLY_START);
        break;

    case COZE_APP_EVENT_AUDIO_DATA:
        // 将音频数据转发给AI对话层处理
        ai_dialog_on_audio_data(event->data, event->length, event->turn, event->sequence);
        break;

    case COZE_APP_EVENT_AUDIO_END:
        ESP_LOGI(TAG, "AI回复结束");
        device_state_send_event(EVENT_AI_REPLY_END);
        break;

    case COZE_APP_EVENT_TURN_END:
        ESP_LOGI(TAG, "对话轮次结束");
        break;

    case COZE_APP_EVENT_ERROR:
        ESP_LOGE(TAG, "Coze错误");
        device_state_send_event(EVENT_COZE_ERROR);
        break;

    case COZE_APP_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "Coze连接断开");
        s_coze_connected = false;
        device_state_send_event(EVENT_BACKEND_DISCONNECTED);
        break;

    default:
        break;
    }
}

// ========== AI对话事件处理 ==========

static void app_on_ai_dialog_event(const ai_dialog_event_t *event, void *context)
{
    (void)context;
    if (event == NULL) {
        return;
    }

    switch (event->type) {
    case AI_DIALOG_EVENT_READY:
        ESP_LOGI(TAG, "AI对话服务就绪");
        device_state_send_event(EVENT_AI_READY);
        break;

    case AI_DIALOG_EVENT_LISTENING:
        ESP_LOGI(TAG, "AI监听用户语音");
        break;

    case AI_DIALOG_EVENT_USER_SPEAKING:
        ESP_LOGI(TAG, "检测到用户说话");
        device_state_send_event(EVENT_USER_SPEAKING);
        break;

    case AI_DIALOG_EVENT_USER_STOPPED:
        ESP_LOGI(TAG, "用户停止说话");
        device_state_send_event(EVENT_USER_STOPPED);
        break;

    case AI_DIALOG_EVENT_SESSION_END:
        ESP_LOGI(TAG, "AI会话结束");
        device_state_send_event(EVENT_CONVERSATION_ENDED);
        break;

    case AI_DIALOG_EVENT_ERROR:
        ESP_LOGE(TAG, "AI对话错误：%s", event->message ? event->message : "未知");
        device_state_send_event(EVENT_COZE_ERROR);
        break;

    default:
        break;
    }
}

// ========== 唤醒词事件处理 ==========

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
        ESP_LOGI(TAG, "检测到唤醒词，概率：%.3f", event->probability);
        device_state_send_event(EVENT_WAKE_WORD_DETECTED);
        break;

    case WAKE_WORD_EVENT_AUDIO_ERROR:
    case WAKE_WORD_EVENT_MODEL_ERROR:
        ESP_LOGE(TAG, "唤醒词错误：%s", esp_err_to_name(event->error));
        device_state_send_event(EVENT_AUDIO_ERROR);
        break;

    case WAKE_WORD_EVENT_TIMEOUT:
        ESP_LOGD(TAG, "唤醒识别窗口结束");
        break;

    case WAKE_WORD_EVENT_STOPPED:
        ESP_LOGI(TAG, "唤醒词采集已停止");
        break;

    default:
        break;
    }
}

// ========== 麦克风PCM数据转发 ==========

static void app_on_capture_pcm(
    const int16_t *samples,
    size_t sample_count,
    void *user_context
)
{
    (void)user_context;

    // 根据当前状态决定PCM数据去向
    device_state_t state = device_state_get_current();

    if (state == DEVICE_STATE_AI_LISTENING ||
        state == DEVICE_STATE_AI_PROCESSING ||
        state == DEVICE_STATE_AI_PLAYING) {
        // AI对话状态，转发给AI对话层
        ai_dialog_on_pcm(samples, sample_count, NULL);
    }
    // TODO: 通话状态转发给通话组件
}

// ========== 语音通话事件处理 ==========

static void app_on_voice_call_event(const voice_call_event_t *event, void *context)
{
    (void)context;
    if (event == NULL) {
        return;
    }

    switch (event->type) {
    case VOICE_CALL_EVENT_INCOMING:
        ESP_LOGI(TAG, "语音来电：%s", event->caller_id);
        break;

    case VOICE_CALL_EVENT_CONNECTED:
        ESP_LOGI(TAG, "语音通话已接通");
        break;

    case VOICE_CALL_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "语音通话已断开");
        break;

    case VOICE_CALL_EVENT_ERROR:
        ESP_LOGE(TAG, "语音通话错误：%s", event->message);
        break;

    default:
        break;
    }
}

// ========== 视频通话事件处理 ==========

static void app_on_video_call_event(const video_call_event_t *event, void *context)
{
    (void)context;
    if (event == NULL) {
        return;
    }

    switch (event->type) {
    case VIDEO_CALL_EVENT_INCOMING:
        ESP_LOGI(TAG, "视频来电：%s", event->caller_id);
        break;

    case VIDEO_CALL_EVENT_CONNECTED:
        ESP_LOGI(TAG, "视频通话已接通");
        break;

    case VIDEO_CALL_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "视频通话已断开");
        break;

    case VIDEO_CALL_EVENT_ERROR:
        ESP_LOGE(TAG, "视频通话错误：%s", event->message);
        break;

    default:
        break;
    }
}

// ========== 设备状态机回调函数 ==========

static void on_network_connecting(void)
{
    ESP_LOGI(TAG, "[状态机] 网络连接中");
    // Wi-Fi已经在启动时初始化
}

static void on_service_starting(void)
{
    ESP_LOGI(TAG, "[状态机] 启动后台服务");

    // 初始化Coze应用
    coze_app_config_t coze_config = {
        .on_event = app_on_coze_event,
        .context = NULL,
    };
    ESP_ERROR_CHECK(coze_app_init(&coze_config));

    // 连接到Coze服务（后台长连接）
    esp_err_t error = coze_app_connect();
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Coze连接失败：%s", esp_err_to_name(error));
        device_state_send_event(EVENT_COZE_ERROR);
        return;
    }

    // 初始化AI对话服务
    ai_dialog_config_t ai_config = {
        .on_event = app_on_ai_dialog_event,
        .context = NULL,
    };
    ESP_ERROR_CHECK(ai_dialog_init(&ai_config));

    // Coze READY事件会触发SERVICE_READY
}

static void on_enter_idle(void)
{
    ESP_LOGI(TAG, "[状态机] 进入待机状态");

    // 检查wake_word服务状态
    wake_word_state_t state = wake_word_get_state();

    if (state == WAKE_WORD_STATE_STOPPED || state == WAKE_WORD_STATE_ERROR) {
        // wake_word未运行或出错，需要重新启动
        esp_err_t error = wake_word_start();
        if (error != ESP_OK) {
            ESP_LOGE(TAG, "唤醒词启动失败：%s", esp_err_to_name(error));
            return;
        }
    } else {
        // wake_word已运行，只需恢复检测
        // 注意：音频资源已在on_conversation_end()中切换完成
        // 这里直接恢复检测即可
        wake_word_resume_detection();
    }
}

static void on_ai_starting(void)
{
    ESP_LOGI(TAG, "[状态机] AI对话启动中");

    // 1. 暂停唤醒词检测（但保持PCM采集）
    wake_word_pause_detection();

    // 2. 标记进入对话模式（必须在切换音频资源之前，避免时序问题）
    wake_word_start_conversation();

    // 3. 切换音频资源给AI对话
    audio_service_transfer(AUDIO_SERVICE_OWNER_WAKE_WORD, AUDIO_SERVICE_OWNER_AI_DIALOG);

    // 开始AI会话
    s_ai_turn++;
    esp_err_t error = ai_dialog_start_session();
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "AI会话启动失败：%s", esp_err_to_name(error));
        device_state_send_event(EVENT_COZE_ERROR);
        return;
    }

    // 开始新的对话轮次
    error = coze_app_begin_turn(s_ai_turn);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Coze轮次启动失败：%s", esp_err_to_name(error));
        device_state_send_event(EVENT_COZE_ERROR);
        return;
    }

    // 播放提示音后发送AI_READY事件
    device_state_send_event(EVENT_AI_READY);
}

static void on_ai_listening(void)
{
    ESP_LOGI(TAG, "[状态机] AI监听用户");
    // AI对话层会自动处理PCM上传
}

static void on_ai_processing(void)
{
    ESP_LOGI(TAG, "[状态机] AI处理中");
    // 用户停止说话，等待AI回复
}

static void on_ai_playing(void)
{
    ESP_LOGI(TAG, "[状态机] AI播放回复");

    // 主动通知AI对话层进入播放状态
    // 这会驱动 ai_dialog_logic 状态切换，确保解码任务可以处理音频数据
    ai_dialog_on_reply_start();
}

static void on_conversation_end(void)
{
    ESP_LOGI(TAG, "[状态机] 对话结束，返回待机");

    // 注意：ai_dialog已经通过finish()发送了SESSION_END事件
    // 这里不需要再调用ai_dialog_stop_session()，避免重复清理

    // 确保Coze轮次结束
    coze_app_end_turn(s_ai_turn);

    // 1. 先标记退出对话模式（改变wake_word的内部标记）
    wake_word_stop_conversation();

    // 2. 然后切换音频资源所有权（在进入IDLE之前完成）
    esp_err_t error = audio_service_transfer(
        AUDIO_SERVICE_OWNER_AI_DIALOG,
        AUDIO_SERVICE_OWNER_WAKE_WORD
    );
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "音频资源切换失败：%s", esp_err_to_name(error));
    }

    // 3. 状态机会自动转换到IDLE，触发on_enter_idle()恢复唤醒词检测
}

static void on_voice_call_incoming(void)
{
    ESP_LOGI(TAG, "[状态机] 语音来电");

    // 如果正在AI对话，先停止
    if (ai_dialog_is_session_active()) {
        ai_dialog_stop_session("来电打断");
        coze_app_end_turn(s_ai_turn);
    }

    // 播放铃声
    voice_call_ring("Unknown");

    // 自动接听（简化处理）
    vTaskDelay(pdMS_TO_TICKS(1000));
    device_state_send_event(EVENT_CALL_ACCEPTED);
}

static void on_voice_call_start(void)
{
    ESP_LOGI(TAG, "[状态机] 语音通话开始");

    // 接听通话
    voice_call_accept();

    // 切换音频资源
    audio_service_transfer(AUDIO_SERVICE_OWNER_WAKE_WORD, AUDIO_SERVICE_OWNER_AI_DIALOG);
}

static void on_voice_call_end(void)
{
    ESP_LOGI(TAG, "[状态机] 语音通话结束");

    // 挂断通话
    voice_call_hangup();

    // 返回待机状态会自动恢复唤醒词
}

static void on_video_call_incoming(void)
{
    ESP_LOGI(TAG, "[状态机] 视频来电");

    // 如果正在AI对话，先停止
    if (ai_dialog_is_session_active()) {
        ai_dialog_stop_session("视频来电打断");
        coze_app_end_turn(s_ai_turn);
    }

    // 播放铃声
    video_call_ring("Unknown");

    // 自动接听（简化处理）
    vTaskDelay(pdMS_TO_TICKS(1000));
    device_state_send_event(EVENT_CALL_ACCEPTED);
}

static void on_video_call_start(void)
{
    ESP_LOGI(TAG, "[状态机] 视频通话开始");

    // 接听视频通话
    video_call_accept();

    // 切换音频资源
    audio_service_transfer(AUDIO_SERVICE_OWNER_WAKE_WORD, AUDIO_SERVICE_OWNER_AI_DIALOG);
}

static void on_video_call_end(void)
{
    ESP_LOGI(TAG, "[状态机] 视频通话结束");

    // 挂断视频通话
    video_call_hangup();

    // 返回待机状态会自动恢复唤醒词
}

static void on_error_recovery(void)
{
    ESP_LOGI(TAG, "[状态机] 错误恢复");

    // 停止所有业务
    if (ai_dialog_is_session_active()) {
        ai_dialog_stop_session("错误恢复");
    }
    if (voice_call_is_active()) {
        voice_call_hangup();
    }
    if (video_call_is_active()) {
        video_call_hangup();
    }

    // 尝试重连Coze
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_err_t error = coze_app_reconnect();
    if (error == ESP_OK) {
        device_state_send_event(EVENT_RECOVERY_COMPLETE);
    } else {
        // 继续重试
        vTaskDelay(pdMS_TO_TICKS(5000));
        device_state_send_event(EVENT_RECOVERY_COMPLETE);
    }
}

static void on_state_changed(device_state_t old_state, device_state_t new_state)
{
    ESP_LOGI(TAG, "[状态变化] %s -> %s",
             device_state_get_name(old_state),
             device_state_get_name(new_state));

    // TODO: 更新LED指示灯、UI等
}

// ========== WiFi事件处理 ==========

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        ESP_LOGI(TAG, "Wi-Fi已连接");
        device_state_send_event(EVENT_WIFI_CONNECTED);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ESP_LOGI(TAG, "获取IP地址");
        device_state_send_event(EVENT_NETWORK_READY);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "Wi-Fi断开");
        device_state_send_event(EVENT_NETWORK_LOST);
    }
}

// ========== 主入口 ==========

void app_main(void)
{
    ESP_LOGI(TAG, "========== 设备启动 - 完整架构版本 ==========");

    // 初始化NVS
    app_initialize_nvs();

    // 初始化事件循环
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // 初始化设备状态机
    device_state_callbacks_t state_callbacks = {
        .on_network_connecting = on_network_connecting,
        .on_service_starting = on_service_starting,
        .on_enter_idle = on_enter_idle,
        .on_ai_starting = on_ai_starting,
        .on_ai_listening = on_ai_listening,
        .on_ai_processing = on_ai_processing,
        .on_ai_playing = on_ai_playing,
        .on_conversation_end = on_conversation_end,
        .on_voice_call_incoming = on_voice_call_incoming,
        .on_voice_call_start = on_voice_call_start,
        .on_voice_call_end = on_voice_call_end,
        .on_video_call_incoming = on_video_call_incoming,
        .on_video_call_start = on_video_call_start,
        .on_video_call_end = on_video_call_end,
        .on_error_recovery = on_error_recovery,
        .on_state_changed = on_state_changed,
    };

    if (!device_state_machine_init(&state_callbacks)) {
        ESP_LOGE(TAG, "状态机初始化失败");
        return;
    }

    // 注册Wi-Fi事件处理
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                &wifi_event_handler, NULL));

    // 启动Wi-Fi配网
    esp_err_t error = p4_c5_wifi_start();
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi启动失败：%s", esp_err_to_name(error));
        return;
    }

    // 初始化统一音频服务
    ESP_ERROR_CHECK(audio_service_init());

    // 初始化唤醒词服务
    ESP_ERROR_CHECK(wake_word_register_callback(app_on_wake_word_event, NULL));
    ESP_ERROR_CHECK(wake_word_register_pcm_callback(app_on_capture_pcm, NULL));
    error = wake_word_init();
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "唤醒词初始化失败：%s", esp_err_to_name(error));
        return;
    }

    // 初始化通话组件
    voice_call_config_t voice_config = {
        .on_event = app_on_voice_call_event,
        .context = NULL,
    };
    ESP_ERROR_CHECK(voice_call_init(&voice_config));

    video_call_config_t video_config = {
        .on_event = app_on_video_call_event,
        .context = NULL,
    };
    ESP_ERROR_CHECK(video_call_init(&video_config));

    ESP_LOGI(TAG, "========== 初始化完成，当前状态: %s ==========",
             device_state_get_name(device_state_get_current()));

    // Wi-Fi连接后会自动触发状态转换
    // BOOT -> NETWORK_CONNECTING -> SERVICE_STARTING -> IDLE
}
