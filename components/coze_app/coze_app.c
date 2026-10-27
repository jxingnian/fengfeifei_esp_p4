/*
 * @file coze_app.c
 * @brief Coze 直连应用组件。
 *
 * 本组件是当前固件唯一的 AI 后端。它只负责 Coze 凭据、实时会话生命周期、
 * Coze 音频协议参数和事件适配；音频设备及 PCM 播放由 audio_service 负责，
 * AI 会话状态和队列由 ai_dialog 负责，三层之间没有后台网关分支。
 */
#include "coze_app.h"

#include <string.h>

#include "esp_coze_chat.h"
#include "esp_log.h"

static const char *TAG = "coze_app";

/* Coze 直连上行使用 60 ms Opus；下行在线路上使用 20 ms Opus。 */
#define COZE_APP_SAMPLE_RATE_HZ          16000U
#define COZE_APP_CHANNELS                1U
#define COZE_APP_BITS_PER_SAMPLE         16U
#define COZE_APP_INPUT_FRAME_MS          60U
#define COZE_APP_OUTPUT_FRAME_MS         20U
#define COZE_APP_OUTPUT_LIMIT_PERIOD_S   1
#define COZE_APP_OUTPUT_MAX_FRAMES       50

static coze_app_config_t s_config;
static esp_coze_chat_handle_t s_chat;
static unsigned s_turn;
static unsigned s_sequence;
static bool s_audio_started;
static bool s_connected;
static bool s_initialized;
static bool s_ready;

/**
 * @brief 将组件事件转发给 AI 对话层。
 *
 * Coze SDK 回调中的音频缓冲只在回调期间有效；本函数不保存指针，
 * ai_dialog 收到事件后会立即复制到自己的有界队列。
 */
static void coze_app_emit(
    coze_app_event_type_t type,
    const uint8_t *data,
    size_t length,
    unsigned sequence
)
{
    if (s_config.on_event == NULL) {
        return;
    }

    coze_app_event_t event = {
        .type = type,
        .turn = s_turn,
        .sequence = sequence,
        .data = data,
        .length = length,
    };
    s_config.on_event(&event, s_config.context);
}

/**
 * @brief 接收 Coze SDK 完成 Base64 解码后的下行 Opus 帧。
 *
 * esp_coze_chat.h 中的 decoded bytes 指 Base64 解码后的字节，不代表已经完成
 * Opus 解码。当前回调只把完整 Opus 帧转交给 ai_dialog，不能在 WebSocket 任务
 * 中执行解码或阻塞等待播放。
 */
static void coze_app_audio_callback(
    const uint8_t *data,
    int length,
    void *context
)
{
    (void)context;
    if (data == NULL || length <= 0) {
        return;
    }

    if (!s_audio_started) {
        s_audio_started = true;
        coze_app_emit(COZE_APP_EVENT_AUDIO_START, NULL, 0, 0);
    }

    s_sequence++;
    coze_app_emit(
        COZE_APP_EVENT_AUDIO_DATA,
        data,
        (size_t)length,
        s_sequence
    );
}

/**
 * @brief 接收 Coze 会话和 WebSocket 非音频事件。
 */
static void coze_app_event_callback(
    esp_coze_chat_event_t event,
    void *event_data,
    void *context
)
{
    (void)context;
    switch (event) {
    case ESP_COZE_CHAT_EVENT_CHAT_COMPLETED:
        coze_app_emit(COZE_APP_EVENT_AUDIO_END, NULL, 0, s_sequence);
        s_audio_started = false;
        break;

    case ESP_COZE_CHAT_EVENT_CHAT_ERROR:
        if (event_data != NULL) {
            const char *message = (const char *)event_data;
            coze_app_emit(
                COZE_APP_EVENT_ERROR,
                (const uint8_t *)message,
                strlen(message),
                0
            );
        } else {
            coze_app_emit(COZE_APP_EVENT_ERROR, NULL, 0, 0);
        }
        break;

    case ESP_COZE_CHAT_EVENT_WS_EVENT:
        if (event_data != NULL) {
            const esp_coze_ws_event_t *ws_event =
                (const esp_coze_ws_event_t *)event_data;
            if (ws_event->event_id == WEBSOCKET_EVENT_CONNECTED) {
                s_connected = true;
            } else if (ws_event->event_id == WEBSOCKET_EVENT_DISCONNECTED) {
                s_connected = false;
                coze_app_emit(COZE_APP_EVENT_DISCONNECTED, NULL, 0, 0);
            }
        }
        break;

    default:
        /* 字幕、客户数据和 SDK 内部事件不改变音频流水线状态。 */
        break;
    }
}

esp_err_t coze_app_init(const coze_app_config_t *config)
{
    if (config == NULL || config->on_event == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (CONFIG_COZE_APP_BOT_ID[0] == '\0' ||
        CONFIG_COZE_APP_ACCESS_TOKEN[0] == '\0' ||
        CONFIG_COZE_APP_USER_ID[0] == '\0') {
        ESP_LOGE(TAG, "Coze 配置为空：Bot ID、Access Token、User ID 均不能为空");
        return ESP_ERR_INVALID_ARG;
    }

    s_config = *config;
    esp_coze_chat_config_t chat_config = ESP_COZE_CHAT_DEFAULT_CONFIG();
    chat_config.bot_id = CONFIG_COZE_APP_BOT_ID;
    chat_config.access_token = CONFIG_COZE_APP_ACCESS_TOKEN;
    chat_config.user_id = CONFIG_COZE_APP_USER_ID;
    chat_config.mode = ESP_COZE_CHAT_SPEECH_INTERRUPT_MODE;
    chat_config.uplink_audio_type = ESP_COZE_CHAT_AUDIO_TYPE_OPUS;
    chat_config.downlink_audio_type = ESP_COZE_CHAT_AUDIO_TYPE_OPUS;
    chat_config.downlink_bitrate = 16000;
    chat_config.downlink_frame_size_ms = COZE_APP_OUTPUT_FRAME_MS;
    chat_config.downlink_limit_period = COZE_APP_OUTPUT_LIMIT_PERIOD_S;
    /* 20 ms 每秒至少需要 50 帧，避免 SDK 默认 18 帧造成必然截断。 */
    chat_config.downlink_max_frame_num = COZE_APP_OUTPUT_MAX_FRAMES;
    chat_config.downlink_speech_rate = 0;
    chat_config.audio_callback = coze_app_audio_callback;
    chat_config.event_callback = coze_app_event_callback;

    esp_err_t error = esp_coze_chat_init(&chat_config, &s_chat);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Coze 直连组件初始化失败：%s", esp_err_to_name(error));
        return error;
    }

    s_connected = false;
    s_ready = false;
    s_initialized = true;
    ESP_LOGI(TAG, "Coze 直连应用已初始化：上行 Opus 60 ms，下行 Opus 20 ms，缓冲上限 50 帧");
    return ESP_OK;
}

esp_err_t coze_app_start(void)
{
    if (!s_initialized || s_chat == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t error = esp_coze_chat_start(s_chat);
    if (error != ESP_OK) {
        s_connected = false;
        s_ready = false;
        coze_app_emit(COZE_APP_EVENT_ERROR, NULL, 0, 0);
        return error;
    }
    s_connected = true;
    s_ready = true;
    return ESP_OK;
}

esp_err_t coze_app_begin_turn(unsigned turn)
{
    if (!s_initialized || !s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    s_turn = turn;
    s_sequence = 0;
    s_audio_started = false;
    return ESP_OK;
}

esp_err_t coze_app_send_audio(const uint8_t *data, size_t length)
{
    if (!s_initialized || !s_ready || data == NULL || length == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    return esp_coze_chat_send_audio_data(s_chat, (char *)data, (int)length);
}

esp_err_t coze_app_commit_audio(unsigned turn)
{
    if (!s_initialized || !s_ready || turn != s_turn) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_coze_chat_send_audio_complete(s_chat);
}

esp_err_t coze_app_cancel(unsigned turn)
{
    if (!s_initialized || !s_ready || turn != s_turn) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_coze_chat_send_audio_cancel(s_chat);
}

void coze_app_stop(void)
{
    if (!s_initialized || s_chat == NULL) {
        return;
    }
    (void)esp_coze_chat_stop(s_chat);
    s_connected = false;
    s_ready = false;
    s_audio_started = false;
}

void coze_app_playback_progress(unsigned turn, unsigned sequence)
{
    /* Coze 直连 SDK 不需要应用层回传播放确认，参数保留用于统一 AI 对话接口。 */
    (void)turn;
    (void)sequence;
}

bool coze_app_is_ready(void)
{
    return s_ready;
}

bool coze_app_connection_is_alive(void)
{
    return s_ready && s_connected;
}

esp_err_t coze_app_get_audio_config(coze_app_audio_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    config->input_opus = true;
    /* Coze SDK 回调返回 Base64 解码后的 Opus，应用层负责实时解码为 PCM。 */
    config->output_opus = true;
    config->input_sample_rate = COZE_APP_SAMPLE_RATE_HZ;
    config->output_sample_rate = COZE_APP_SAMPLE_RATE_HZ;
    config->input_channels = COZE_APP_CHANNELS;
    config->output_channels = COZE_APP_CHANNELS;
    config->input_bits_per_sample = COZE_APP_BITS_PER_SAMPLE;
    config->output_bits_per_sample = COZE_APP_BITS_PER_SAMPLE;
    config->input_frame_us = COZE_APP_INPUT_FRAME_MS * 1000U;
    config->output_frame_us = COZE_APP_OUTPUT_FRAME_MS * 1000U;
    config->server_vad = false;
    return ESP_OK;
}
