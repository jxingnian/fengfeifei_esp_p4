/**
 * @file voice_ai_backend.c
 * @brief 云端 AI 和 Coze 直连 AI 的统一适配实现。
 *
 * 本文件是项目应用层代码，不属于生成的 esp_coze 组件。所有供应商 API
 * 调用集中在这里，状态机和音频应用流程只看到统一的后端接口。
 */

#include "voice_ai_backend.h"

#include <stdio.h>
#include <string.h>

#include "cloud_gateway.h"
#include "esp_log.h"

#if CONFIG_VOICE_AI_BACKEND_COZE_DIRECT
#include "esp_coze_chat.h"
#endif

static const char *TAG = "voice_ai_backend";
static voice_ai_backend_config_t s_config;
static bool s_initialized;
static bool s_ready;

#if CONFIG_VOICE_AI_BACKEND_COZE_DIRECT
static esp_coze_chat_handle_t s_coze_chat;
static unsigned s_coze_turn;
static unsigned s_coze_sequence;
static bool s_coze_audio_started;
static bool s_coze_connected;

static void coze_emit(voice_ai_backend_event_type_t type,
                      const uint8_t *data,
                      size_t length,
                      unsigned sequence)
{
    if (s_config.on_event == NULL) {
        return;
    }

    voice_ai_backend_event_t event = {
        .type = type,
        .turn = s_coze_turn,
        .sequence = sequence,
        .data = data,
        .length = length,
    };
    s_config.on_event(&event, s_config.context);
}

static void coze_audio_callback(const uint8_t *data, int length, void *context)
{
    (void)context;
    if (data == NULL || length <= 0) {
        return;
    }

    /* 第一帧下行音频就是应用层进入播放状态的明确依据。 */
    if (!s_coze_audio_started) {
        s_coze_audio_started = true;
        coze_emit(VOICE_AI_BACKEND_EVENT_AUDIO_START, NULL, 0, 0);
    }

    s_coze_sequence++;
    coze_emit(VOICE_AI_BACKEND_EVENT_AUDIO_DATA,
              data,
              (size_t)length,
              s_coze_sequence);
}

static void coze_event_callback(esp_coze_chat_event_t event,
                                void *event_data,
                                void *context)
{
    (void)context;
    switch (event) {
    case ESP_COZE_CHAT_EVENT_CHAT_COMPLETED:
        /* 当前组件将 conversation.chat.completed 作为回复音频结束边界。 */
        coze_emit(VOICE_AI_BACKEND_EVENT_AUDIO_END, NULL, 0, s_coze_sequence);
        s_coze_audio_started = false;
        break;

    case ESP_COZE_CHAT_EVENT_CHAT_ERROR:
        if (event_data != NULL) {
            const char *message = (const char *)event_data;
            coze_emit(VOICE_AI_BACKEND_EVENT_ERROR,
                      (const uint8_t *)message,
                      strlen(message),
                      0);
        } else {
            coze_emit(VOICE_AI_BACKEND_EVENT_ERROR, NULL, 0, 0);
        }
        break;

    case ESP_COZE_CHAT_EVENT_WS_EVENT:
        /*
         * esp_coze 组件将底层 WebSocket 事件封装后回调到应用层；应用层只
         * 记录连接是否仍然存在，并把断开事件交给逻辑层统一结束会话。
         */
        if (event_data != NULL) {
            const esp_coze_ws_event_t *ws_event =
                (const esp_coze_ws_event_t *)event_data;
            if (ws_event->event_id == WEBSOCKET_EVENT_CONNECTED) {
                s_coze_connected = true;
            } else if (ws_event->event_id == WEBSOCKET_EVENT_DISCONNECTED) {
                s_coze_connected = false;
                coze_emit(VOICE_AI_BACKEND_EVENT_DISCONNECTED, NULL, 0, 0);
            }
        }
        break;

    default:
        /* 字幕、未知事件和 WebSocket 内部事件不改变应用状态。 */
        break;
    }
}
#endif

esp_err_t voice_ai_backend_init(const voice_ai_backend_config_t *config)
{
    if (config == NULL || config->on_event == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    s_config = *config;

#if CONFIG_VOICE_AI_BACKEND_COZE_DIRECT
    /*
     * 空字符串会让 esp_coze 初始化成功，但传输层会因此跳过
     * Authorization 请求头。这里在应用层提前拒绝，避免再次建立无鉴权连接。
     */
    if (CONFIG_VOICE_AI_COZE_BOT_ID[0] == '\0' ||
        CONFIG_VOICE_AI_COZE_ACCESS_TOKEN[0] == '\0' ||
        CONFIG_VOICE_AI_COZE_USER_ID[0] == '\0') {
        ESP_LOGE(TAG, "Coze 配置为空：Bot ID、Access Token 和 User ID 均不能为空");
        return ESP_ERR_INVALID_ARG;
    }

    esp_coze_chat_config_t chat_config = ESP_COZE_CHAT_DEFAULT_CONFIG();
    chat_config.bot_id = CONFIG_VOICE_AI_COZE_BOT_ID;
    chat_config.access_token = CONFIG_VOICE_AI_COZE_ACCESS_TOKEN;
    chat_config.user_id = CONFIG_VOICE_AI_COZE_USER_ID;
    chat_config.mode = ESP_COZE_CHAT_SPEECH_INTERRUPT_MODE;
    chat_config.uplink_audio_type = ESP_COZE_CHAT_AUDIO_TYPE_OPUS;
    chat_config.downlink_audio_type = ESP_COZE_CHAT_AUDIO_TYPE_OPUS;
    chat_config.downlink_bitrate = 16000;
    chat_config.downlink_frame_size_ms = 20;
    chat_config.audio_callback = coze_audio_callback;
    chat_config.event_callback = coze_event_callback;

    esp_err_t error = esp_coze_chat_init(&chat_config, &s_coze_chat);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Coze 后端初始化失败：%s", esp_err_to_name(error));
        return error;
    }
    ESP_LOGI(TAG, "AI 后端：Coze 直连模式");
#else
    ESP_LOGI(TAG, "AI 后端：云端网关模式");
#endif

    s_ready = false;
    s_coze_connected = false;
    s_initialized = true;
    return ESP_OK;
}

esp_err_t voice_ai_backend_start(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

#if CONFIG_VOICE_AI_BACKEND_COZE_DIRECT
    esp_err_t error = esp_coze_chat_start(s_coze_chat);
    if (error != ESP_OK) {
        s_coze_connected = false;
        coze_emit(VOICE_AI_BACKEND_EVENT_ERROR, NULL, 0, 0);
        return error;
    }
    s_coze_connected = true;
    s_ready = true;
    return ESP_OK;
#else
    cloud_gateway_status_t status = {0};
    if (cloud_gateway_get_status(&status) != ESP_OK || status.state != CLOUD_GATEWAY_READY) {
        return ESP_ERR_INVALID_STATE;
    }
    s_ready = true;
    return ESP_OK;
#endif
}

esp_err_t voice_ai_backend_begin_turn(unsigned turn)
{
    if (!s_initialized || !s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

#if CONFIG_VOICE_AI_BACKEND_COZE_DIRECT
    s_coze_turn = turn;
    s_coze_sequence = 0;
    s_coze_audio_started = false;
    return ESP_OK;
#else
    char message[128];
    int length = snprintf(message, sizeof(message),
                          "{\"type\":\"audio_start\",\"turnId\":%u}", turn);
    return cloud_gateway_send(false, message, (size_t)length);
#endif
}

esp_err_t voice_ai_backend_send_audio(const uint8_t *data, size_t length)
{
    if (!s_initialized || !s_ready || data == NULL || length == 0) {
        return ESP_ERR_INVALID_ARG;
    }

#if CONFIG_VOICE_AI_BACKEND_COZE_DIRECT
    return esp_coze_chat_send_audio_data(s_coze_chat, (char *)data, (int)length);
#else
    return cloud_gateway_send(true, data, length);
#endif
}

esp_err_t voice_ai_backend_commit_audio(unsigned turn)
{
    if (!s_initialized || !s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

#if CONFIG_VOICE_AI_BACKEND_COZE_DIRECT
    (void)turn;
    return esp_coze_chat_send_audio_complete(s_coze_chat);
#else
    char message[128];
    int length = snprintf(message, sizeof(message),
                          "{\"type\":\"audio_end\",\"turnId\":%u}", turn);
    return cloud_gateway_send_audio_end(message, (size_t)length);
#endif
}

esp_err_t voice_ai_backend_cancel(unsigned turn)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

#if CONFIG_VOICE_AI_BACKEND_COZE_DIRECT
    (void)turn;
    if (!s_ready) {
        return ESP_OK;
    }
    return esp_coze_chat_send_audio_cancel(s_coze_chat);
#else
    char message[128];
    int length = snprintf(message, sizeof(message),
                          "{\"type\":\"cancel\",\"turnId\":%u}", turn);
    return cloud_gateway_send(false, message, (size_t)length);
#endif
}

void voice_ai_backend_stop(void)
{
    if (!s_initialized) {
        return;
    }

#if CONFIG_VOICE_AI_BACKEND_COZE_DIRECT
    if (s_coze_chat != NULL) {
        (void)esp_coze_chat_stop(s_coze_chat);
    }
    s_coze_connected = false;
    s_ready = false;
#else
    cloud_gateway_discard_audio();
    s_ready = false;
#endif
}

void voice_ai_backend_playback_progress(unsigned turn, unsigned sequence)
{
#if CONFIG_VOICE_AI_BACKEND_COZE_DIRECT
    (void)turn;
    (void)sequence;
#else
    char message[160];
    int length = snprintf(message, sizeof(message),
                          "{\"type\":\"playback_progress\",\"turnId\":%u,\"sequence\":%u}",
                          turn, sequence);
    if (cloud_gateway_send(false, message, (size_t)length) != ESP_OK) {
        ESP_LOGW(TAG, "云端播放进度发送失败：turn=%u sequence=%u", turn, sequence);
    }
#endif
}

bool voice_ai_backend_is_direct(void)
{
#if CONFIG_VOICE_AI_BACKEND_COZE_DIRECT
    return true;
#else
    return false;
#endif
}

bool voice_ai_backend_is_ready(void)
{
    return s_ready; /* 应用层后端启动标志，不等同于底层连接状态。 */
}

bool voice_ai_backend_connection_is_alive(void)
{
#if CONFIG_VOICE_AI_BACKEND_COZE_DIRECT
    /* Coze 组件的发送接口会在断链后返回错误；这里先依据会话生命周期判断。 */
    /* s_ready 表示应用层已启动后端，s_coze_connected 才表示 WebSocket 仍在线。 */
    return s_ready && s_coze_connected;
#else
    cloud_gateway_status_t status = {0};
    return cloud_gateway_get_status(&status) == ESP_OK &&
           status.state == CLOUD_GATEWAY_READY;
#endif
}

esp_err_t voice_ai_backend_get_audio_config(voice_ai_backend_audio_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

#if CONFIG_VOICE_AI_BACKEND_COZE_DIRECT
    config->input_opus = true;
    config->output_opus = true;
    config->output_frame_us = 20000;
    config->server_vad = false;
    return ESP_OK;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

void voice_ai_backend_on_cloud_message(bool binary, const void *data, size_t length)
{
    (void)binary;
    (void)data;
    (void)length;
    /* 云端原始协议仍由应用层兼容入口处理；直连模式不会消费云端 AI 消息。 */
}
