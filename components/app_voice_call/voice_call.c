/**
 * @file voice_call.c
 * @brief 语音通话业务组件实现（占位实现）
 */

#include "voice_call.h"
#include "esp_log.h"
#include "audio_service.h"
#include <string.h>

static const char *TAG = "voice_call";

static voice_call_config_t s_config = {0};
static bool s_initialized = false;
static bool s_is_active = false;
static char s_caller_id[64] = {0};

esp_err_t voice_call_init(const voice_call_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_initialized) {
        ESP_LOGW(TAG, "语音通话服务已初始化");
        return ESP_OK;
    }

    s_config = *config;
    s_initialized = true;
    s_is_active = false;
    ESP_LOGI(TAG, "语音通话服务初始化成功");
    return ESP_OK;
}

void voice_call_deinit(void)
{
    if (!s_initialized) {
        return;
    }

    if (s_is_active) {
        voice_call_hangup();
    }

    memset(&s_config, 0, sizeof(s_config));
    s_initialized = false;
    ESP_LOGI(TAG, "语音通话服务已反初始化");
}

esp_err_t voice_call_ring(const char *caller_id)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (caller_id) {
        strncpy(s_caller_id, caller_id, sizeof(s_caller_id) - 1);
    } else {
        strcpy(s_caller_id, "Unknown");
    }

    ESP_LOGI(TAG, "来电振铃：%s", s_caller_id);

    // TODO: 播放铃声
    // audio_service_play_ringtone();

    if (s_config.on_event) {
        voice_call_event_t event = {
            .type = VOICE_CALL_EVENT_INCOMING,
            .caller_id = s_caller_id,
            .message = NULL,
        };
        s_config.on_event(&event, s_config.context);
    }

    return ESP_OK;
}

esp_err_t voice_call_accept(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "接听通话");

    // 切换音频资源所有权
    // audio_service_acquire(AUDIO_SERVICE_OWNER_VOICE_CALL, portMAX_DELAY);

    s_is_active = true;

    // TODO: 建立通话连接
    // xn_connector_accept_call();

    if (s_config.on_event) {
        voice_call_event_t event = {
            .type = VOICE_CALL_EVENT_CONNECTED,
            .caller_id = s_caller_id,
            .message = NULL,
        };
        s_config.on_event(&event, s_config.context);
    }

    return ESP_OK;
}

esp_err_t voice_call_reject(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "拒接通话");

    // TODO: 拒接通话
    // xn_connector_reject_call();

    memset(s_caller_id, 0, sizeof(s_caller_id));
    return ESP_OK;
}

esp_err_t voice_call_hangup(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!s_is_active) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "挂断通话");

    s_is_active = false;

    // TODO: 挂断通话
    // xn_connector_hangup();

    // 释放音频资源
    // audio_service_release(AUDIO_SERVICE_OWNER_VOICE_CALL);

    if (s_config.on_event) {
        voice_call_event_t event = {
            .type = VOICE_CALL_EVENT_DISCONNECTED,
            .caller_id = s_caller_id,
            .message = NULL,
        };
        s_config.on_event(&event, s_config.context);
    }

    memset(s_caller_id, 0, sizeof(s_caller_id));
    return ESP_OK;
}

bool voice_call_is_active(void)
{
    return s_is_active;
}
