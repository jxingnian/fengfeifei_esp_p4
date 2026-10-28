/**
 * @file video_uplink.c
 * @brief ESP32-P4 MIPI-CSI 视频采集和 JPEG 硬件编码实现。
 */
#include "video_uplink.h"
#include <fcntl.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include "bsp/esp-bsp.h"
#include "driver/jpeg_encode.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "linux/videodev2.h"

static const char *TAG = "video_uplink";
#define DEFAULT_BUFFER_COUNT 3
#define DEFAULT_JPEG_SIZE (512U * 1024U)
#define DEFAULT_STACK_SIZE 8192
#define DEFAULT_PRIORITY 5

typedef struct {
    video_uplink_config_t config;
    int fd;
    TaskHandle_t task;
    jpeg_encoder_handle_t encoder;
    uint8_t **buffers;
    size_t *buffer_lengths;
    uint8_t *jpeg_buffer;
    size_t jpeg_buffer_size;
    uint32_t width;
    uint32_t height;
    size_t raw_size;
    uint32_t sequence;
    volatile bool stop_requested;
    bool initialized;
    bool running;
    video_uplink_stats_t stats;
} video_context_t;

static video_context_t s_context = {.fd = -1};

void video_uplink_config_default(video_uplink_config_t *config)
{
    if (config == NULL) return;
    memset(config, 0, sizeof(*config));
    config->frame_rate = 30;
    config->pixel_format = VIDEO_UPLINK_PIXEL_RGB565;
    config->buffer_count = DEFAULT_BUFFER_COUNT;
    config->jpeg_quality = 75;
    config->jpeg_buffer_size = DEFAULT_JPEG_SIZE;
    config->task_core = -1;
    config->task_priority = DEFAULT_PRIORITY;
    config->task_stack_size = DEFAULT_STACK_SIZE;
}

static uint32_t v4l2_format(video_uplink_pixel_format_t format)
{
    return format == VIDEO_UPLINK_PIXEL_RGB888 ? V4L2_PIX_FMT_RGB24 : V4L2_PIX_FMT_RGB565;
}

static jpeg_enc_input_format_t jpeg_format(video_uplink_pixel_format_t format)
{
    return format == VIDEO_UPLINK_PIXEL_RGB888 ? JPEG_ENCODE_IN_FORMAT_RGB888 : JPEG_ENCODE_IN_FORMAT_RGB565;
}

static esp_err_t open_camera(void)
{
    struct v4l2_capability capability = {0};
    struct v4l2_format format = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE};
    s_context.fd = open(BSP_CAMERA_DEVICE, O_RDONLY);
    if (s_context.fd < 0) return ESP_FAIL;
    if (ioctl(s_context.fd, VIDIOC_QUERYCAP, &capability) != 0 || ioctl(s_context.fd, VIDIOC_G_FMT, &format) != 0) {
        return ESP_FAIL;
    }
    if (s_context.config.width != 0) format.fmt.pix.width = s_context.config.width;
    if (s_context.config.height != 0) format.fmt.pix.height = s_context.config.height;
    format.fmt.pix.pixelformat = v4l2_format(s_context.config.pixel_format);
    if (ioctl(s_context.fd, VIDIOC_S_FMT, &format) != 0 || ioctl(s_context.fd, VIDIOC_G_FMT, &format) != 0) return ESP_FAIL;
    if (format.fmt.pix.pixelformat != v4l2_format(s_context.config.pixel_format)) return ESP_ERR_NOT_SUPPORTED;
    s_context.width = format.fmt.pix.width;
    s_context.height = format.fmt.pix.height;
    s_context.raw_size = format.fmt.pix.sizeimage;
    if (s_context.raw_size == 0) s_context.raw_size = s_context.width * s_context.height * (s_context.config.pixel_format == VIDEO_UPLINK_PIXEL_RGB888 ? 3U : 2U);
    ESP_LOGI(TAG, "摄像头已打开: %lux%lu, size=%u, driver=%s", (unsigned long)s_context.width, (unsigned long)s_context.height, (unsigned)s_context.raw_size, capability.driver);
    return ESP_OK;
}

static esp_err_t allocate_capture_buffers(void)
{
    struct v4l2_requestbuffers request = {0};
    request.count = s_context.config.buffer_count;
    request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    request.memory = V4L2_MEMORY_USERPTR;
    if (ioctl(s_context.fd, VIDIOC_REQBUFS, &request) != 0 || request.count < s_context.config.buffer_count) return ESP_FAIL;
    s_context.buffers = calloc(s_context.config.buffer_count, sizeof(uint8_t *));
    s_context.buffer_lengths = calloc(s_context.config.buffer_count, sizeof(size_t));
    if (s_context.buffers == NULL || s_context.buffer_lengths == NULL) return ESP_ERR_NO_MEM;
    for (uint8_t i = 0; i < s_context.config.buffer_count; ++i) {
        struct v4l2_buffer buffer = {.type = request.type, .memory = request.memory, .index = i};
        if (ioctl(s_context.fd, VIDIOC_QUERYBUF, &buffer) != 0) return ESP_FAIL;
        s_context.buffers[i] = heap_caps_aligned_calloc(64, 1, s_context.raw_size, MALLOC_CAP_SPIRAM);
        if (s_context.buffers[i] == NULL) return ESP_ERR_NO_MEM;
        s_context.buffer_lengths[i] = s_context.raw_size;
        buffer.m.userptr = (unsigned long)s_context.buffers[i];
        buffer.length = s_context.raw_size;
        if (ioctl(s_context.fd, VIDIOC_QBUF, &buffer) != 0) return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t create_encoder(void)
{
    const jpeg_encode_engine_cfg_t engine_config = {.timeout_ms = 100};
    const jpeg_encode_memory_alloc_cfg_t memory_config = {.buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER};
    size_t allocated = 0;
    esp_err_t ret = jpeg_new_encoder_engine(&engine_config, &s_context.encoder);
    if (ret != ESP_OK) return ret;
    s_context.jpeg_buffer = jpeg_alloc_encoder_mem(s_context.config.jpeg_buffer_size, &memory_config, &allocated);
    if (s_context.jpeg_buffer == NULL) return ESP_ERR_NO_MEM;
    s_context.jpeg_buffer_size = allocated;
    return ESP_OK;
}

static void release_resources(void)
{
    if (s_context.fd >= 0) {
        int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        (void)ioctl(s_context.fd, VIDIOC_STREAMOFF, &type);
        close(s_context.fd);
        s_context.fd = -1;
    }
    if (s_context.encoder != NULL) {
        (void)jpeg_del_encoder_engine(s_context.encoder);
        s_context.encoder = NULL;
    }
    if (s_context.jpeg_buffer != NULL) {
        heap_caps_free(s_context.jpeg_buffer);
        s_context.jpeg_buffer = NULL;
    }
    if (s_context.buffers != NULL) {
        for (uint8_t i = 0; i < s_context.config.buffer_count; ++i) {
            if (s_context.buffers[i] != NULL) heap_caps_free(s_context.buffers[i]);
        }
        free(s_context.buffers);
        s_context.buffers = NULL;
    }
    free(s_context.buffer_lengths);
    s_context.buffer_lengths = NULL;
}

static void capture_task(void *arg)
{
    (void)arg;
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(s_context.fd, VIDIOC_STREAMON, &type) != 0) {
        s_context.running = false;
        s_context.task = NULL;
        vTaskDelete(NULL);
        return;
    }
    s_context.running = true;
    while (!s_context.stop_requested) {
        struct v4l2_buffer buffer = {.type = type, .memory = V4L2_MEMORY_USERPTR};
        if (ioctl(s_context.fd, VIDIOC_DQBUF, &buffer) != 0 || buffer.index >= s_context.config.buffer_count) {
            s_context.stats.dropped_frames++;
            continue;
        }
        uint64_t timestamp = esp_timer_get_time();
        uint32_t sequence = s_context.sequence++;
        s_context.stats.captured_frames++;
        video_uplink_raw_frame_t raw = {
            .data = s_context.buffers[buffer.index], .length = buffer.bytesused,
            .width = s_context.width, .height = s_context.height,
            .format = s_context.config.pixel_format, .timestamp_us = timestamp, .sequence = sequence,
        };
        if (s_context.config.raw_frame_cb != NULL) s_context.config.raw_frame_cb(&raw, s_context.config.user_ctx);
        jpeg_encode_cfg_t encode_config = {
            .src_type = jpeg_format(s_context.config.pixel_format),
            .sub_sample = JPEG_DOWN_SAMPLING_YUV422,
            .image_quality = s_context.config.jpeg_quality,
            .width = s_context.width, .height = s_context.height,
        };
        uint32_t jpeg_size = 0;
        int64_t start = esp_timer_get_time();
        esp_err_t ret = jpeg_encoder_process(s_context.encoder, &encode_config, raw.data, raw.length, s_context.jpeg_buffer, s_context.jpeg_buffer_size, &jpeg_size);
        s_context.stats.last_encode_time_us = (uint32_t)(esp_timer_get_time() - start);
        if (ret == ESP_OK) {
            s_context.stats.encoded_frames++;
            s_context.stats.last_jpeg_size = jpeg_size;
            video_uplink_jpeg_frame_t jpeg = {
                .data = s_context.jpeg_buffer, .length = jpeg_size,
                .width = s_context.width, .height = s_context.height,
                .timestamp_us = timestamp, .sequence = sequence,
            };
            s_context.config.jpeg_frame_cb(&jpeg, s_context.config.user_ctx);
        } else {
            s_context.stats.encode_errors++;
        }
        buffer.m.userptr = (unsigned long)s_context.buffers[buffer.index];
        buffer.length = s_context.buffer_lengths[buffer.index];
        if (ioctl(s_context.fd, VIDIOC_QBUF, &buffer) != 0) s_context.stats.dropped_frames++;
    }
    (void)ioctl(s_context.fd, VIDIOC_STREAMOFF, &type);
    s_context.running = false;
    s_context.task = NULL;
    vTaskDelete(NULL);
}

esp_err_t video_uplink_init(const video_uplink_config_t *config)
{
    if (config == NULL || config->jpeg_frame_cb == NULL || s_context.initialized) return ESP_ERR_INVALID_ARG;
    s_context.config = *config;
    if (s_context.config.buffer_count == 0) s_context.config.buffer_count = DEFAULT_BUFFER_COUNT;
    if (s_context.config.jpeg_quality == 0) s_context.config.jpeg_quality = 75;
    if (s_context.config.jpeg_buffer_size == 0) s_context.config.jpeg_buffer_size = DEFAULT_JPEG_SIZE;
    if (s_context.config.task_core < -1) return ESP_ERR_INVALID_ARG;
    if (s_context.config.buffer_count < 2 || s_context.config.buffer_count > 3 || s_context.config.jpeg_quality > 100) return ESP_ERR_INVALID_ARG;
    esp_err_t ret = bsp_camera_start(NULL);
    if (ret == ESP_OK) ret = open_camera();
    if (ret == ESP_OK) ret = allocate_capture_buffers();
    if (ret == ESP_OK) ret = create_encoder();
    if (ret != ESP_OK) {
        release_resources();
        return ret;
    }
    s_context.initialized = true;
    return ESP_OK;
}

esp_err_t video_uplink_start(void)
{
    if (!s_context.initialized || s_context.task != NULL) return ESP_ERR_INVALID_STATE;
    s_context.stop_requested = false;
    BaseType_t result;
    if (s_context.config.task_core >= 0) {
        result = xTaskCreatePinnedToCore(capture_task, "video_uplink", s_context.config.task_stack_size ? s_context.config.task_stack_size : DEFAULT_STACK_SIZE, NULL, s_context.config.task_priority ? s_context.config.task_priority : DEFAULT_PRIORITY, &s_context.task, s_context.config.task_core);
    } else {
        result = xTaskCreate(capture_task, "video_uplink", s_context.config.task_stack_size ? s_context.config.task_stack_size : DEFAULT_STACK_SIZE, NULL, s_context.config.task_priority ? s_context.config.task_priority : DEFAULT_PRIORITY, &s_context.task);
    }
    return result == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t video_uplink_stop(void)
{
    if (!s_context.initialized) return ESP_ERR_INVALID_STATE;
    s_context.stop_requested = true;
    for (int i = 0; i < 100 && s_context.task != NULL; ++i) vTaskDelay(pdMS_TO_TICKS(10));
    return s_context.task == NULL ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t video_uplink_deinit(void)
{
    if (!s_context.initialized) return ESP_ERR_INVALID_STATE;
    esp_err_t ret = video_uplink_stop();
    if (ret != ESP_OK) return ret;
    release_resources();
    memset(&s_context, 0, sizeof(s_context));
    s_context.fd = -1;
    return ESP_OK;
}

esp_err_t video_uplink_get_stats(video_uplink_stats_t *stats)
{
    if (!s_context.initialized || stats == NULL) return ESP_ERR_INVALID_ARG;
    *stats = s_context.stats;
    return ESP_OK;
}
