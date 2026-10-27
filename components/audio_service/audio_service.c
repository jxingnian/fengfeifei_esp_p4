/*
 * @file audio_service.c
 * @brief 统一音频服务实现。
 *
 * 本文件是业务组件与 ES8311 驱动之间唯一的边界。资源锁和 I/O 锁分开
 * 管理：资源锁记录业务所有权，I/O 锁保证底层 codec 的一次读写不会与
 * 另一条任务并发访问，从而为后续接入通话和系统提示音保留明确扩展点。
 */
#include "audio_service.h"

#include "audio_driver.h"
#include "esp_afe_aec.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/semphr.h"
#include <stdlib.h>
#include <string.h>

#define AUDIO_SERVICE_AEC_REFERENCE_SAMPLES (32768U)
#define AUDIO_SERVICE_AEC_MAX_FRAME_SAMPLES (1024U)

static const char *TAG = "audio_service";

static SemaphoreHandle_t s_owner_mutex;
static SemaphoreHandle_t s_io_mutex;
static SemaphoreHandle_t s_reference_mutex;
static audio_service_owner_t s_owner = AUDIO_SERVICE_OWNER_NONE;
static bool s_initialized;
static bool s_capture_active;
static afe_aec_handle_t *s_aec;
static int16_t *s_aec_input;
static int16_t *s_aec_output;
static int16_t *s_aec_mic;
static int16_t *s_aec_reference;
static int16_t *s_reference_ring;
static size_t s_aec_frame_samples;
static size_t s_aec_pending_offset;
static size_t s_aec_pending_samples;
static size_t s_reference_read;
static size_t s_reference_write;
static size_t s_reference_count;

/**
 * @brief 清空扬声器播放参考环形缓冲区。
 *
 * AEC 的参考信号必须与麦克风采集时间轴连续。扬声器停止后清除旧回答的
 * 参考 PCM，后续读取参考时自动使用静音样本。
 */
static void audio_service_clear_reference(void)
{
    if (s_reference_mutex == NULL || s_reference_ring == NULL) {
        return;
    }
    xSemaphoreTake(s_reference_mutex, portMAX_DELAY);
    s_reference_read = 0;
    s_reference_write = 0;
    s_reference_count = 0;
    xSemaphoreGive(s_reference_mutex);
}

/**
 * @brief 写入扬声器播放参考 PCM。
 *
 * 参考缓冲区满时丢弃最旧参考数据；参考数据只影响 AEC 收敛，不允许阻塞
 * 实际扬声器播放任务。
 */
static void audio_service_push_reference(const int16_t *samples, size_t count)
{
    if (samples == NULL || count == 0 || s_reference_mutex == NULL || s_reference_ring == NULL) {
        return;
    }
    xSemaphoreTake(s_reference_mutex, portMAX_DELAY);
    for (size_t i = 0; i < count; ++i) {
        if (s_reference_count == AUDIO_SERVICE_AEC_REFERENCE_SAMPLES) {
            s_reference_read = (s_reference_read + 1U) % AUDIO_SERVICE_AEC_REFERENCE_SAMPLES;
            s_reference_count--;
        }
        s_reference_ring[s_reference_write] = samples[i];
        s_reference_write = (s_reference_write + 1U) % AUDIO_SERVICE_AEC_REFERENCE_SAMPLES;
        s_reference_count++;
    }
    xSemaphoreGive(s_reference_mutex);
}

/**
 * @brief 读取播放参考 PCM，参考不足时使用静音填充。
 */
static void audio_service_pop_reference(int16_t *samples, size_t count)
{
    memset(samples, 0, count * sizeof(int16_t));
    if (s_reference_mutex == NULL || s_reference_ring == NULL) {
        return;
    }
    xSemaphoreTake(s_reference_mutex, portMAX_DELAY);
    for (size_t i = 0; i < count && s_reference_count != 0; ++i) {
        samples[i] = s_reference_ring[s_reference_read];
        s_reference_read = (s_reference_read + 1U) % AUDIO_SERVICE_AEC_REFERENCE_SAMPLES;
        s_reference_count--;
    }
    xSemaphoreGive(s_reference_mutex);
}

/**
 * @brief 创建 ESP-SR AFE AEC 实例和工作缓冲区。
 */
static esp_err_t audio_service_init_aec(void)
{
#if CONFIG_IDF_TARGET_ESP32P4
    const int filter_length = 4;
#else
    const int filter_length = 2;
#endif
    s_aec = afe_aec_create("MR", filter_length, AFE_TYPE_FD, AFE_MODE_HIGH_PERF);
    if (s_aec == NULL) {
        return ESP_ERR_NO_MEM;
    }
    const int frame_samples = afe_aec_get_chunksize(s_aec);
    if (frame_samples <= 0 || (unsigned)frame_samples > AUDIO_SERVICE_AEC_MAX_FRAME_SAMPLES) {
        return ESP_ERR_INVALID_SIZE;
    }
    s_aec_frame_samples = (size_t)frame_samples;
    s_aec_input = heap_caps_aligned_calloc(
        16, s_aec_frame_samples * 2U, sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_aec_output = heap_caps_aligned_calloc(
        16, s_aec_frame_samples, sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_aec_mic = heap_caps_aligned_calloc(
        16, s_aec_frame_samples, sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_aec_reference = heap_caps_aligned_calloc(
        16, s_aec_frame_samples, sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_reference_ring = heap_caps_calloc(
        AUDIO_SERVICE_AEC_REFERENCE_SAMPLES, sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_aec_input == NULL || s_aec_output == NULL || s_aec_mic == NULL ||
        s_aec_reference == NULL || s_reference_ring == NULL) {
        return ESP_ERR_NO_MEM;
    }
    audio_service_clear_reference();
    return ESP_OK;
}

/**
 * @brief 释放 ESP-SR AFE AEC 资源。
 */
static void audio_service_deinit_aec(void)
{
    if (s_aec != NULL) {
        afe_aec_destroy(s_aec);
        s_aec = NULL;
    }
    /* 对齐分配的内存使用 ESP-IDF 推荐的通用能力内存释放接口。 */
    heap_caps_free(s_aec_input);
    heap_caps_free(s_aec_output);
    heap_caps_free(s_aec_mic);
    heap_caps_free(s_aec_reference);
    free(s_reference_ring);
    s_aec_input = NULL;
    s_aec_output = NULL;
    s_aec_mic = NULL;
    s_aec_reference = NULL;
    s_reference_ring = NULL;
    s_aec_frame_samples = 0;
    s_aec_pending_offset = 0;
    s_aec_pending_samples = 0;
}

static bool audio_service_valid_owner(audio_service_owner_t owner)
{
    return owner > AUDIO_SERVICE_OWNER_NONE &&
           owner <= AUDIO_SERVICE_OWNER_SYSTEM_ALERT;
}

static esp_err_t audio_service_check_owner(audio_service_owner_t owner)
{
    if (!s_initialized || !audio_service_valid_owner(owner)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_owner_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    const bool owned = s_owner == owner;
    xSemaphoreGive(s_owner_mutex);
    return owned ? ESP_OK : ESP_ERR_INVALID_STATE;
}

static esp_err_t audio_service_io_lock(audio_service_owner_t owner)
{
    esp_err_t error = audio_service_check_owner(owner);
    if (error != ESP_OK) {
        return error;
    }
    if (xSemaphoreTake(s_io_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t audio_service_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    s_owner_mutex = xSemaphoreCreateMutex();
    s_io_mutex = xSemaphoreCreateMutex();
    s_reference_mutex = xSemaphoreCreateMutex();
    if (s_owner_mutex == NULL || s_io_mutex == NULL || s_reference_mutex == NULL) {
        if (s_owner_mutex != NULL) {
            vSemaphoreDelete(s_owner_mutex);
            s_owner_mutex = NULL;
        }
        if (s_io_mutex != NULL) {
            vSemaphoreDelete(s_io_mutex);
            s_io_mutex = NULL;
        }
        if (s_reference_mutex != NULL) {
            vSemaphoreDelete(s_reference_mutex);
            s_reference_mutex = NULL;
        }
        return ESP_ERR_NO_MEM;
    }

    esp_err_t error = audio_driver_init();
    if (error != ESP_OK) {
        vSemaphoreDelete(s_owner_mutex);
        vSemaphoreDelete(s_io_mutex);
        vSemaphoreDelete(s_reference_mutex);
        s_owner_mutex = NULL;
        s_io_mutex = NULL;
        s_reference_mutex = NULL;
        return error;
    }

    error = audio_service_init_aec();
    if (error != ESP_OK) {
        audio_driver_deinit();
        audio_service_deinit_aec();
        vSemaphoreDelete(s_owner_mutex);
        vSemaphoreDelete(s_io_mutex);
        vSemaphoreDelete(s_reference_mutex);
        s_owner_mutex = NULL;
        s_io_mutex = NULL;
        s_reference_mutex = NULL;
        return error;
    }

    s_owner = AUDIO_SERVICE_OWNER_NONE;
    s_capture_active = false;
    s_initialized = true;
    ESP_LOGI(TAG, "统一音频服务已启动：16 kHz、单声道、16 bit、扬声器音量 100%%");
    return ESP_OK;
}

esp_err_t audio_service_deinit(void)
{
    if (!s_initialized) {
        return ESP_OK;
    }
    if (s_owner != AUDIO_SERVICE_OWNER_NONE || s_capture_active) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t error = audio_driver_deinit();
    audio_service_deinit_aec();
    vSemaphoreDelete(s_owner_mutex);
    vSemaphoreDelete(s_io_mutex);
    vSemaphoreDelete(s_reference_mutex);
    s_owner_mutex = NULL;
    s_io_mutex = NULL;
    s_reference_mutex = NULL;
    s_owner = AUDIO_SERVICE_OWNER_NONE;
    s_capture_active = false;
    s_initialized = false;
    return error;
}

esp_err_t audio_service_get_format(audio_service_format_t *format)
{
    if (!s_initialized || format == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    format->sample_rate_hz = 16000;
    format->channels = 1;
    format->bits_per_sample = 16;
    return ESP_OK;
}

esp_err_t audio_service_acquire(audio_service_owner_t owner, TickType_t timeout_ticks)
{
    if (!s_initialized || !audio_service_valid_owner(owner)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(s_owner_mutex, timeout_ticks) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t error = ESP_OK;
    if (s_owner == AUDIO_SERVICE_OWNER_NONE || s_owner == owner) {
        s_owner = owner;
    } else {
        error = ESP_ERR_INVALID_STATE;
    }
    xSemaphoreGive(s_owner_mutex);
    return error;
}

esp_err_t audio_service_release(audio_service_owner_t owner)
{
    if (!s_initialized || !audio_service_valid_owner(owner)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(s_owner_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t error = ESP_OK;
    if (s_owner == owner && !s_capture_active) {
        s_owner = AUDIO_SERVICE_OWNER_NONE;
    } else if (s_owner != owner) {
        error = ESP_ERR_INVALID_STATE;
    } else {
        error = ESP_ERR_INVALID_STATE;
    }
    xSemaphoreGive(s_owner_mutex);
    return error;
}

esp_err_t audio_service_transfer(audio_service_owner_t from, audio_service_owner_t to)
{
    if (!s_initialized || !audio_service_valid_owner(from) ||
        !audio_service_valid_owner(to)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(s_owner_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t error = s_owner == from ? ESP_OK : ESP_ERR_INVALID_STATE;
    if (error == ESP_OK) {
        s_owner = to;
    }
    xSemaphoreGive(s_owner_mutex);
    return error;
}

esp_err_t audio_service_start_capture(audio_service_owner_t owner)
{
    esp_err_t error = audio_service_acquire(owner, portMAX_DELAY);
    if (error != ESP_OK) {
        return error;
    }
    s_capture_active = true;
    return ESP_OK;
}

esp_err_t audio_service_stop_capture(audio_service_owner_t owner)
{
    if (audio_service_check_owner(owner) != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    s_capture_active = false;
    return ESP_OK;
}

esp_err_t audio_service_read_pcm(
    audio_service_owner_t owner,
    int16_t *samples,
    size_t sample_count
)
{
    if (samples == NULL || sample_count == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t error = audio_service_io_lock(owner);
    if (error != ESP_OK) {
        return error;
    }
    size_t produced = 0;
    while (produced < sample_count) {
        /* 先消费上一次 AEC 帧未被调用方取完的净化 PCM。 */
        if (s_aec_pending_samples != 0) {
            size_t copy_count = s_aec_pending_samples;
            if (copy_count > sample_count - produced) {
                copy_count = sample_count - produced;
            }
            memcpy(samples + produced,
                   s_aec_output + s_aec_pending_offset,
                   copy_count * sizeof(int16_t));
            produced += copy_count;
            s_aec_pending_offset += copy_count;
            s_aec_pending_samples -= copy_count;
            if (s_aec_pending_samples == 0) {
                s_aec_pending_offset = 0;
            }
            continue;
        }

        /* AEC 输入必须是交错的 M(麦克风)R(播放参考)双通道 PCM。 */
        error = audio_driver_read(s_aec_mic, s_aec_frame_samples);
        if (error != ESP_OK) {
            break;
        }
        audio_service_pop_reference(s_aec_reference, s_aec_frame_samples);
        for (size_t i = 0; i < s_aec_frame_samples; ++i) {
            s_aec_input[i * 2U] = s_aec_mic[i];
            s_aec_input[i * 2U + 1U] = s_aec_reference[i];
        }
        const size_t output_bytes = afe_aec_process(s_aec, s_aec_input, s_aec_output);
        if (output_bytes != s_aec_frame_samples * sizeof(int16_t)) {
            error = ESP_FAIL;
            break;
        }
        s_aec_pending_samples = s_aec_frame_samples;
    }
    xSemaphoreGive(s_io_mutex);
    return error;
}

esp_err_t audio_service_write_pcm(
    audio_service_owner_t owner,
    const int16_t *samples,
    size_t sample_count
)
{
    if (samples == NULL || sample_count == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t error = audio_service_io_lock(owner);
    if (error != ESP_OK) {
        return error;
    }
    error = audio_driver_write(samples, sample_count);
    if (error == ESP_OK) {
        /* 只有真正写入驱动的 PCM 才能作为扬声器回声参考。 */
        audio_service_push_reference(samples, sample_count);
    }
    xSemaphoreGive(s_io_mutex);
    return error;
}

esp_err_t audio_service_set_output_enabled(audio_service_owner_t owner, bool enabled)
{
    esp_err_t error = audio_service_io_lock(owner);
    if (error != ESP_OK) {
        return error;
    }
    error = audio_driver_set_output_enabled(enabled);
    if (error == ESP_OK && !enabled) {
        audio_service_clear_reference();
    }
    xSemaphoreGive(s_io_mutex);
    return error;
}

esp_err_t audio_service_drain(audio_service_owner_t owner)
{
    esp_err_t error = audio_service_io_lock(owner);
    if (error != ESP_OK) {
        return error;
    }
    error = audio_driver_drain();
    xSemaphoreGive(s_io_mutex);
    return error;
}
