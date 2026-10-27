/*
 * @file audio_driver.h
 * @brief ESP32-P4-WIFI6-DB 的最小 ES8311 麦克风适配接口。
 *
 * 本组件只保留语音识别需要的 I2C、I2S 和 ES8311 ADC 初始化代码，
 * 不依赖开发板完整 BSP，因此不会拉入显示、触摸、摄像头和视频组件。
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 ES8311 麦克风和 I2S 输入链路。
 *
 * 初始化后的输出格式固定为 16 kHz、16 bit、单声道 PCM。
 */
esp_err_t audio_driver_init(void);

/**
 * @brief 从 ES8311 读取指定数量的 16 bit PCM 样本。
 */
esp_err_t audio_driver_read(int16_t *samples, size_t sample_count);

/** Write 16 kHz, 16 bit, mono PCM to the ES8311 speaker. */
esp_err_t audio_driver_write(const int16_t *samples, size_t sample_count);

esp_err_t audio_driver_set_output_enabled(bool enabled);
/** Flush the bounded DMA ring with silence before turning off the amplifier. */
esp_err_t audio_driver_drain(void);

/**
 * @brief 关闭并释放音频资源。
 */
esp_err_t audio_driver_deinit(void);

#ifdef __cplusplus
}
#endif
