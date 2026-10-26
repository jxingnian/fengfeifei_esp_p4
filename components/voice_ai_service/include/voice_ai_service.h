#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化设备端 AI 实时语音会话服务。
 *
 * 该服务只负责 Coze 实时语音会话的状态管理、音频上行和音频下行，
 * 不负责设备配对，也不直接保存或访问 Coze Token。Token 始终只保留在后台。
 */
esp_err_t voice_ai_service_init(void);
/** Startup-only PCM then Opus tone test; microphone/RTC must be stopped. */
esp_err_t voice_ai_service_audio_self_test(void);

/**
 * @brief 处理本地 MultiNet 已检测到唤醒词的事件。
 *
 * 回调可能运行在语音识别任务中，因此函数必须快速返回；真正的音频播放
 * 和后台消息处理由 AI 服务自己的 FreeRTOS 任务执行。
 */
void voice_ai_service_on_wake_detected(void);

/**
 * @brief 接收统一麦克风采集任务输出的一帧 PCM 数据。
 *
 * 数据格式由设备硬件固定为 16 kHz、单声道、16 bit little-endian PCM。
 * 函数只在本轮监听中拼接 60ms PCM 并零等待入队，不执行 Opus 编码、
 * 网络发送或播放；编码和发送入队交给独立的大栈任务。
 */
void voice_ai_service_on_pcm(const int16_t *samples, size_t sample_count, void *context);

/**
 * @brief 接收后台设备 WebSocket 的文字事件或二进制音频。
 *
 * cloud_gateway 的回调数据只在回调期间有效，本函数会复制需要异步处理的内容。
 */
void voice_ai_service_on_cloud_message(bool binary, const void *data, size_t length);

/**
 * @brief 主动结束当前 AI 会话并恢复本地唤醒监听。
 *
 * 视频通话抢占麦克风和扬声器时调用该接口，保证通话音频不会与 AI 音频混流。
 */
void voice_ai_service_stop_session(const char *reason);

/**
 * @brief 查询当前是否处于唤醒后的 AI 会话。
 */
bool voice_ai_service_is_session_active(void);

#ifdef __cplusplus
}
#endif
