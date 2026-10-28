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
    COZE_APP_EVENT_CONNECTED = 0,       // WebSocket连接成功
    COZE_APP_EVENT_READY,               // Coze服务就绪，可以开始对话
    COZE_APP_EVENT_AUDIO_START,         // AI开始回复音频
    COZE_APP_EVENT_AUDIO_DATA,          // AI音频数据
    COZE_APP_EVENT_AUDIO_END,           // AI音频结束
    COZE_APP_EVENT_TURN_END,            // 本轮对话结束
    COZE_APP_EVENT_ERROR,               // 错误
    COZE_APP_EVENT_DISCONNECTED,        // 连接断开
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

/**
 * @brief 初始化Coze应用组件（不建立连接）
 */
esp_err_t coze_app_init(const coze_app_config_t *config);

/**
 * @brief 连接到Coze服务（后台长连接）
 *
 * 该函数在网络就绪后调用一次，建立WebSocket连接并保持
 */
esp_err_t coze_app_connect(void);

/**
 * @brief 重连Coze服务（网络恢复后）
 */
esp_err_t coze_app_reconnect(void);

/**
 * @brief 开始新一轮对话（唤醒后调用）
 */
esp_err_t coze_app_begin_turn(unsigned turn);

/**
 * @brief 发送音频数据
 */
esp_err_t coze_app_send_audio(const uint8_t *data, size_t length);

/**
 * @brief 提交音频（用户停止说话）
 */
esp_err_t coze_app_commit_audio(unsigned turn);

/**
 * @brief 取消当前轮次
 */
esp_err_t coze_app_cancel(unsigned turn);

/**
 * @brief 结束当前轮次（但不断开连接）
 */
esp_err_t coze_app_end_turn(unsigned turn);

/**
 * @brief 断开连接并停止服务
 */
void coze_app_disconnect(void);

/**
 * @brief 反初始化
 */
void coze_app_deinit(void);

/**
 * @brief 播放进度反馈
 */
void coze_app_playback_progress(unsigned turn, unsigned sequence);

/**
 * @brief 检查Coze服务是否就绪
 */
bool coze_app_is_ready(void);

/**
 * @brief 检查连接是否存活
 */
bool coze_app_is_connected(void);

/**
 * @brief 获取音频配置
 */
esp_err_t coze_app_get_audio_config(coze_app_audio_config_t *config);

#ifdef __cplusplus
}
#endif
