/**
 * @file video_uplink.h
 * @brief ESP32-P4 摄像头采集和 JPEG 视频上行组件接口。
 *
 * 组件只负责 MIPI-CSI 采集、PSRAM 缓冲和 JPEG 编码，不负责屏幕、解码、
 * 音频、网络连接和通话信令。JPEG 回调返回后，帧缓存由组件继续复用。
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 摄像头输出像素格式。 */
typedef enum {
    VIDEO_UPLINK_PIXEL_RGB565 = 1, /**< 每像素 2 字节。 */
    VIDEO_UPLINK_PIXEL_RGB888 = 2, /**< 每像素 3 字节。 */
} video_uplink_pixel_format_t;

/** 原始帧描述，raw_frame_cb 返回后不得继续使用 data。 */
typedef struct {
    const uint8_t *data; /**< 原始像素数据地址。 */
    size_t length; /**< 原始像素数据长度。 */
    uint32_t width; /**< 图像宽度。 */
    uint32_t height; /**< 图像高度。 */
    video_uplink_pixel_format_t format; /**< 像素格式。 */
    uint64_t timestamp_us; /**< 采集时间戳。 */
    uint32_t sequence; /**< 帧序号。 */
} video_uplink_raw_frame_t;

/** JPEG 帧描述，jpeg_frame_cb 返回后不得继续使用 data。 */
typedef struct {
    const uint8_t *data; /**< JPEG 数据地址。 */
    size_t length; /**< JPEG 数据长度。 */
    uint32_t width; /**< 原始图像宽度。 */
    uint32_t height; /**< 原始图像高度。 */
    uint64_t timestamp_us; /**< 采集时间戳。 */
    uint32_t sequence; /**< 对应原始帧序号。 */
} video_uplink_jpeg_frame_t;

/** 采集、编码和任务参数。零值使用默认值。 */
typedef struct {
    uint32_t width; /**< 采集宽度，零值使用传感器当前宽度。 */
    uint32_t height; /**< 采集高度，零值使用传感器当前高度。 */
    uint32_t frame_rate; /**< 目标帧率记录值，零值为 30。 */
    video_uplink_pixel_format_t pixel_format; /**< RGB565 或 RGB888。 */
    uint8_t buffer_count; /**< V4L2 用户缓冲数量，范围 2~3。 */
    uint8_t jpeg_quality; /**< JPEG 质量，范围 1~100。 */
    size_t jpeg_buffer_size; /**< JPEG 输出缓存大小，默认 512 KiB。 */
    int task_core; /**< 任务核心，-1 表示不绑定。 */
    int task_priority; /**< 采集任务优先级。 */
    uint32_t task_stack_size; /**< 采集任务栈大小。 */
    void (*raw_frame_cb)(const video_uplink_raw_frame_t *frame, void *user_ctx); /**< 原始帧回调。 */
    void (*jpeg_frame_cb)(const video_uplink_jpeg_frame_t *frame, void *user_ctx); /**< JPEG 帧回调，必填。 */
    void *user_ctx; /**< 回调上下文。 */
} video_uplink_config_t;

/** 视频运行统计。 */
typedef struct {
    uint32_t captured_frames; /**< 已取出的摄像头帧数。 */
    uint32_t encoded_frames; /**< 成功编码帧数。 */
    uint32_t dropped_frames; /**< 丢弃帧数。 */
    uint32_t encode_errors; /**< 编码失败数。 */
    uint32_t last_jpeg_size; /**< 最近 JPEG 大小。 */
    uint32_t last_encode_time_us; /**< 最近编码耗时。 */
} video_uplink_stats_t;

/** 填充默认配置。 */
void video_uplink_config_default(video_uplink_config_t *config);
/** 初始化摄像头、缓冲区和 JPEG 编码器。 */
esp_err_t video_uplink_init(const video_uplink_config_t *config);
/** 启动采集任务。 */
esp_err_t video_uplink_start(void);
/** 请求停止采集任务。 */
esp_err_t video_uplink_stop(void);
/** 停止并释放所有资源。 */
esp_err_t video_uplink_deinit(void);
/** 读取统计数据。 */
esp_err_t video_uplink_get_stats(video_uplink_stats_t *stats);

#ifdef __cplusplus
}
#endif
