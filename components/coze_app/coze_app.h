#pragma once

/**
 * @file coze_app.h
 * @brief Coze 直连 AI 应用组件接口。
 *
 * 该接口只表达当前 Coze 实时语音会话需要的生命周期和音频事件，不暴露
 * esp_coze 的 WebSocket、JSON 或传输细节。后续增加其他 AI 后端时，应新增
 * 独立应用组件，不在本组件中重新引入后台网关分支。
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    COZE_APP_EVENT_AUDIO_START = 0,
    COZE_APP_EVENT_AUDIO_DATA,
    COZE_APP_EVENT_AUDIO_END,
    COZE_APP_EVENT_ERROR,
    COZE_APP_EVENT_DISCONNECTED,
} coze_app_event_type_t;

/**
 * @brief Coze 事件。
 *
 * data 仅在回调期间有效；接收方必须在回调中复制需要异步处理的数据。
 */
typedef struct {
    coze_app_event_type_t type;
    unsigned turn;
    unsigned sequence;
    const uint8_t *data;
    size_t length;
} coze_app_event_t;

typedef void (*coze_app_event_callback_t)(
    const coze_app_event_t *event,
    void *context
);

typedef struct {
    coze_app_event_callback_t on_event;
    void *context;
} coze_app_config_t;

/**
 * @brief AI 对话层使用的统一 PCM/Opus 格式说明。
 *
 * 当前上行字段表示送往 Coze 的 Opus 编码格式；下行字段表示 SDK 回调
 * 交给应用层的线路编码格式。下行使用 Opus 时，应用层必须先解码为 PCM。
 */
typedef struct {
    bool input_opus;
    bool output_opus;
    unsigned input_sample_rate;
    unsigned output_sample_rate;
    unsigned input_channels;
    unsigned output_channels;
    unsigned input_bits_per_sample;
    unsigned output_bits_per_sample;
    unsigned input_frame_us;
    unsigned output_frame_us;
} coze_app_audio_config_t;

esp_err_t coze_app_init(const coze_app_config_t *config);
esp_err_t coze_app_start(void);
esp_err_t coze_app_begin_turn(unsigned turn);
esp_err_t coze_app_send_audio(const uint8_t *data, size_t length);
esp_err_t coze_app_commit_audio(unsigned turn);
esp_err_t coze_app_cancel(unsigned turn);
void coze_app_stop(void);
void coze_app_playback_progress(unsigned turn, unsigned sequence);
bool coze_app_is_ready(void);
bool coze_app_connection_is_alive(void);
esp_err_t coze_app_get_audio_config(coze_app_audio_config_t *config);

#ifdef __cplusplus
}
#endif
