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
#include "esp_log.h"
#include "freertos/semphr.h"

static const char *TAG = "audio_service";

static SemaphoreHandle_t s_owner_mutex;
static SemaphoreHandle_t s_io_mutex;
static audio_service_owner_t s_owner = AUDIO_SERVICE_OWNER_NONE;
static bool s_initialized;
static bool s_capture_active;

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
    if (s_owner_mutex == NULL || s_io_mutex == NULL) {
        if (s_owner_mutex != NULL) {
            vSemaphoreDelete(s_owner_mutex);
            s_owner_mutex = NULL;
        }
        if (s_io_mutex != NULL) {
            vSemaphoreDelete(s_io_mutex);
            s_io_mutex = NULL;
        }
        return ESP_ERR_NO_MEM;
    }

    esp_err_t error = audio_driver_init();
    if (error != ESP_OK) {
        vSemaphoreDelete(s_owner_mutex);
        vSemaphoreDelete(s_io_mutex);
        s_owner_mutex = NULL;
        s_io_mutex = NULL;
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
    vSemaphoreDelete(s_owner_mutex);
    vSemaphoreDelete(s_io_mutex);
    s_owner_mutex = NULL;
    s_io_mutex = NULL;
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
    error = audio_driver_read(samples, sample_count);
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
