#pragma once

/**
 * @file voice_ai_backend.h
 * @brief AI 后端适配层接口。
 *
 * 应用层只通过本接口访问 AI 后端。Coze 组件和云端网关的协议细节均在
 * voice_ai_backend.c 内部实现，逻辑层不依赖任何具体供应商接口。
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    VOICE_AI_BACKEND_EVENT_AUDIO_START = 0,
    VOICE_AI_BACKEND_EVENT_AUDIO_DATA,
    VOICE_AI_BACKEND_EVENT_AUDIO_END,
    VOICE_AI_BACKEND_EVENT_ERROR,
    VOICE_AI_BACKEND_EVENT_DISCONNECTED
} voice_ai_backend_event_type_t;

/**
 * @brief 后端事件回调参数。
 *
 * data 只在回调期间有效。应用层如果需要异步处理，必须立即复制数据。
 */
typedef struct {
    voice_ai_backend_event_type_t type;
    unsigned turn;
    unsigned sequence;
    const uint8_t *data;
    size_t length;
} voice_ai_backend_event_t;

typedef void (*voice_ai_backend_event_callback_t)(
    const voice_ai_backend_event_t *event,
    void *context);

typedef struct {
    voice_ai_backend_event_callback_t on_event;
    void *context;
} voice_ai_backend_config_t;

typedef struct {
    bool input_opus;
    bool output_opus;
    unsigned output_frame_us;
    bool server_vad;
} voice_ai_backend_audio_config_t;

esp_err_t voice_ai_backend_init(const voice_ai_backend_config_t *config);
esp_err_t voice_ai_backend_start(void);
esp_err_t voice_ai_backend_begin_turn(unsigned turn);
esp_err_t voice_ai_backend_send_audio(const uint8_t *data, size_t length);
esp_err_t voice_ai_backend_commit_audio(unsigned turn);
esp_err_t voice_ai_backend_cancel(unsigned turn);
void voice_ai_backend_stop(void);
void voice_ai_backend_playback_progress(unsigned turn, unsigned sequence);
bool voice_ai_backend_is_direct(void);
bool voice_ai_backend_is_ready(void);
bool voice_ai_backend_connection_is_alive(void);
esp_err_t voice_ai_backend_get_audio_config(voice_ai_backend_audio_config_t *config);

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 将云端网关收到的原始消息交给云端后端。
 *
 * Coze 直连模式下该函数直接丢弃消息，防止云端 AI 音频进入本地直连会话。
 */
void voice_ai_backend_on_cloud_message(bool binary, const void *data, size_t length);

#ifdef __cplusplus
}
#endif
