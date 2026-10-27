/*
 * @file audio_service.h
 * @brief 设备统一音频服务接口。
 *
 * audio_driver 只负责 ES8311、I2S 和功放等板级硬件操作；本服务负责把
 * 这些硬件能力封装成可被唤醒词、AI 对话以及未来通话组件复用的业务接口。
 * 上层组件禁止直接包含 audio_driver.h，所有 PCM 读写必须通过本接口完成。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 可以占用音频资源的业务模块。
 *
 * 当前只启用唤醒词和 AI 对话两个所有者；保留未来通话和系统提示音的
 * 枚举值，使后续接入不需要重新设计音频资源管理接口。
 */
typedef enum {
    AUDIO_SERVICE_OWNER_NONE = 0,
    AUDIO_SERVICE_OWNER_WAKE_WORD,
    AUDIO_SERVICE_OWNER_AI_DIALOG,
    AUDIO_SERVICE_OWNER_VIDEO_CALL,
    AUDIO_SERVICE_OWNER_SYSTEM_ALERT,
} audio_service_owner_t;

/**
 * @brief 固定板级 PCM 格式。
 */
typedef struct {
    uint32_t sample_rate_hz;
    uint8_t channels;
    uint8_t bits_per_sample;
} audio_service_format_t;

/**
 * @brief 初始化统一音频服务及板级驱动。
 */
esp_err_t audio_service_init(void);

/**
 * @brief 释放统一音频服务及板级驱动。
 */
esp_err_t audio_service_deinit(void);

/**
 * @brief 查询当前板级 PCM 格式。
 */
esp_err_t audio_service_get_format(audio_service_format_t *format);

/**
 * @brief 获取音频资源所有权。
 *
 * 所有权只保护“当前由哪个业务模块使用音频链路”的状态；单次 PCM 读写
 * 仍由服务内部的 I/O 锁串行化，因此调用方不会长期占用锁而阻塞另一类
 * 音频操作。重复获取同一所有者是幂等操作。
 */
esp_err_t audio_service_acquire(audio_service_owner_t owner, TickType_t timeout_ticks);

/**
 * @brief 释放指定业务模块的音频资源所有权。
 */
esp_err_t audio_service_release(audio_service_owner_t owner);

/**
 * @brief 原子切换音频资源所有权。
 */
esp_err_t audio_service_transfer(
    audio_service_owner_t from,
    audio_service_owner_t to
);

/**
 * @brief 标记开始采集。
 */
esp_err_t audio_service_start_capture(audio_service_owner_t owner);

/**
 * @brief 标记停止采集。
 */
esp_err_t audio_service_stop_capture(audio_service_owner_t owner);

/**
 * @brief 从麦克风读取 PCM 样本。
 */
esp_err_t audio_service_read_pcm(
    audio_service_owner_t owner,
    int16_t *samples,
    size_t sample_count
);

/**
 * @brief 向扬声器写入 PCM 样本。
 */
esp_err_t audio_service_write_pcm(
    audio_service_owner_t owner,
    const int16_t *samples,
    size_t sample_count
);

/**
 * @brief 开关功放，并在打开时恢复 100% 输出音量。
 */
esp_err_t audio_service_set_output_enabled(
    audio_service_owner_t owner,
    bool enabled
);

/**
 * @brief 用静音填满 DMA 尾部，确保关闭功放前没有截断语音。
 */
esp_err_t audio_service_drain(audio_service_owner_t owner);

#ifdef __cplusplus
}
#endif
