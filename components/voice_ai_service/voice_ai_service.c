/*
 * @file voice_ai_service.c
 * @brief 设备端 Coze 实时语音会话实现。
 *
 * 设备端只连接自有后台的 /ws/device，后台再连接 Coze 官方 WebSocket。
 * 本文件不保存 Coze Token，所有上行音频和下行音频都经过 cloud_gateway。
 *
 * 设备音频资源采用半双工策略：
 * 1. 本地 MultiNet 负责持续监听唤醒词；
 * 2. 唤醒后由 voice_service 继续读取同一个麦克风，并把 PCM 分片交给本服务；
 * 3. 后台返回 audio_start 后暂停麦克风任务，避免扬声器回声进入下一轮识别；
 * 4. 收到 audio_end 或 audio_stop 后关闭功放并恢复 AI 采集；
 * 5. 通话开始时由 call_service 结束 AI 会话，保证通话和 AI 不共享实时音频。
 */

#include "voice_ai_service.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "cloud_gateway.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "voice_board_audio.h"
#include "voice_service.h"

/* 后台二进制帧的上限与 cloud_gateway 的单条消息上限保持一致。 */
#define VOICE_AI_MAX_BINARY_MESSAGE 4096U
#define VOICE_AI_MAX_JSON_MESSAGE 16384U
#define VOICE_AI_CONTROL_QUEUE_LENGTH 8U
#define VOICE_AI_AUDIO_QUEUE_LENGTH 32U
#define VOICE_AI_SESSION_TIMEOUT_MS 120000LL

static const char *TAG = "voice_ai_service";

typedef struct {
    bool binary;
    size_t length;
    uint8_t *data;
} voice_ai_message_t;

/**
 * @brief AI 会话失败原因。
 *
 * 以前所有原因都合并成“后台音频发送失败”，无法区分上行、下行和播放
 * 队列问题。这里保留明确原因，日志可以直接指导后续修复和现场排查。
 */
typedef enum {
    VOICE_AI_FAILURE_NONE = 0,
    VOICE_AI_FAILURE_UPLINK_NOT_READY,
    VOICE_AI_FAILURE_UPLINK_QUEUE_FULL,
    VOICE_AI_FAILURE_UPLINK_INVALID_ARGUMENT,
    VOICE_AI_FAILURE_UPLINK_OTHER,
    VOICE_AI_FAILURE_DOWNLINK_CONTROL_TOO_LARGE,
    VOICE_AI_FAILURE_DOWNLINK_AUDIO_TOO_LARGE,
    VOICE_AI_FAILURE_DOWNLINK_CONTROL_ALLOC,
    VOICE_AI_FAILURE_DOWNLINK_AUDIO_ALLOC,
    VOICE_AI_FAILURE_DOWNLINK_CONTROL_QUEUE_FULL,
    VOICE_AI_FAILURE_DOWNLINK_AUDIO_QUEUE_FULL,
    VOICE_AI_FAILURE_CAPTURE_RESTORE,
} voice_ai_failure_t;

static QueueHandle_t s_control_queue;
static QueueHandle_t s_audio_queue;
static atomic_bool s_initialized;
static atomic_bool s_session_active;
static atomic_bool s_playing;
static atomic_bool s_ready;
static atomic_bool s_supported_audio;
static atomic_int s_failure;
static atomic_uint s_ready_epoch;
static atomic_bool s_capture_restore_pending;
static atomic_uint s_ignored_event_count;
static int64_t s_session_deadline_ms;

/* 队列清理函数位于消息释放函数之前，先声明以保持编译器检查有效。 */
static void voice_ai_free_message(voice_ai_message_t *message);

/**
 * @brief 返回单调递增的毫秒时间。
 *
 * esp_timer_get_time() 使用微秒，转换后用于会话超时判断，不依赖系统校时结果。
 */
static int64_t voice_ai_now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

/**
 * @brief 返回失败原因的稳定中文名称。
 */
static const char *voice_ai_failure_name(voice_ai_failure_t failure)
{
    switch (failure) {
    case VOICE_AI_FAILURE_UPLINK_NOT_READY: return "AI 上行连接未就绪";
    case VOICE_AI_FAILURE_UPLINK_QUEUE_FULL: return "AI 上行发送队列已满";
    case VOICE_AI_FAILURE_UPLINK_INVALID_ARGUMENT: return "AI 上行数据参数无效";
    case VOICE_AI_FAILURE_UPLINK_OTHER: return "AI 上行发送失败";
    case VOICE_AI_FAILURE_DOWNLINK_CONTROL_TOO_LARGE: return "AI 下行控制消息过大";
    case VOICE_AI_FAILURE_DOWNLINK_AUDIO_TOO_LARGE: return "AI 下行 PCM 消息过大";
    case VOICE_AI_FAILURE_DOWNLINK_CONTROL_ALLOC: return "AI 下行控制消息内存不足";
    case VOICE_AI_FAILURE_DOWNLINK_AUDIO_ALLOC: return "AI 下行 PCM 消息内存不足";
    case VOICE_AI_FAILURE_DOWNLINK_CONTROL_QUEUE_FULL: return "AI 下行控制队列已满";
    case VOICE_AI_FAILURE_DOWNLINK_AUDIO_QUEUE_FULL: return "AI 下行 PCM 队列已满";
    case VOICE_AI_FAILURE_CAPTURE_RESTORE: return "AI 会话结束后恢复麦克风失败";
    default: return "AI 音频传输失败";
    }
}

/**
 * @brief 只保留第一次失败原因，避免多个任务覆盖现场证据。
 */
static void voice_ai_set_failure(voice_ai_failure_t failure, const char *detail)
{
    int expected = VOICE_AI_FAILURE_NONE;
    if (atomic_compare_exchange_strong(&s_failure, &expected, failure)) {
        ESP_LOGE(TAG, "%s%s%s", voice_ai_failure_name(failure),
                 detail != NULL ? "：" : "", detail != NULL ? detail : "");
    }
}

/**
 * @brief 释放指定队列中尚未消费的消息，避免上一轮 PCM 进入下一轮会话。
 */
static void voice_ai_clear_queue(QueueHandle_t queue)
{
    if (queue == NULL) {
        return;
    }

    voice_ai_message_t message = {0};
    while (xQueueReceive(queue, &message, 0) == pdTRUE) {
        voice_ai_free_message(&message);
        memset(&message, 0, sizeof(message));
    }
}

/**
 * @brief 判断文字消息是否属于设备必须处理的控制协议。
 *
 * Coze 原始事件、文字增量和调试事件均在此处丢弃，不进入设备队列。
 */
static bool voice_ai_is_control_message(const void *data, size_t length)
{
    cJSON *root = cJSON_ParseWithLength((const char *)data, length);
    if (root == NULL) {
        return false;
    }

    const char *type = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "type"));
    bool accepted = type != NULL &&
                    (!strcmp(type, "ready") || !strcmp(type, "audio_start") ||
                     !strcmp(type, "audio_end") || !strcmp(type, "audio_stop") ||
                     !strcmp(type, "error"));
    cJSON_Delete(root);
    return accepted;
}

/**
 * @brief 释放一条队列消息，并清理音频缓冲区中的明文数据。
 */
static void voice_ai_free_message(voice_ai_message_t *message)
{
    if (message == NULL || message->data == NULL) {
        return;
    }

    memset(message->data, 0, message->length);
    free(message->data);
    message->data = NULL;
    message->length = 0;
}

/**
 * @brief 向后台发送一个设备端控制事件。
 *
 * cloud_gateway_send() 使用零等待队列写入，不在这里等待网络发送完成。
 */
static esp_err_t voice_ai_send_event(const char *type)
{
    char message[64];
    int length = snprintf(message, sizeof(message), "{\"type\":\"%s\"}", type);
    if (length <= 0 || (size_t)length >= sizeof(message)) {
        return ESP_ERR_INVALID_SIZE;
    }

    return cloud_gateway_send(false, message, (size_t)length);
}

/**
 * @brief 将设备恢复到采集状态。
 *
 * audio_end/audio_stop 之后，voice_service 任务可能此前已经被暂停；只有确认
 * 当前仍属于 AI 会话时才重新启动，避免通话或连接错误期间重新抢占麦克风。
 */
static void voice_ai_resume_capture(void)
{
    (void)voice_board_audio_set_output_enabled(false);
    atomic_store(&s_playing, false);

    if (atomic_load(&s_session_active)) {
        esp_err_t error = voice_service_start();
        if (error == ESP_OK) {
            atomic_store(&s_capture_restore_pending, false);
        } else if (error == ESP_ERR_INVALID_STATE) {
            /* 任务仍在退出时，交给 AI 工作任务周期性重试。 */
            atomic_store(&s_capture_restore_pending, true);
        } else {
            ESP_LOGE(TAG, "AI 音频播放结束后恢复麦克风失败：%s", esp_err_to_name(error));
            voice_ai_set_failure(VOICE_AI_FAILURE_CAPTURE_RESTORE, esp_err_to_name(error));
        }
    }
}

/**
 * @brief 在语音采集任务退出后重试恢复唤醒监听。
 *
 * voice_service_stop() 超时只代表任务尚未完成退出，并不代表音频链路已经
 * 失效。这里保留恢复请求，直到 voice_service_start() 正式成功，避免设备
 * 因一次任务切换时序冲突永久停在无唤醒状态。
 */
static void voice_ai_retry_capture_restore(void)
{
    if (!atomic_load(&s_capture_restore_pending) || atomic_load(&s_session_active)) {
        return;
    }

    esp_err_t error = voice_service_start();
    if (error == ESP_OK) {
        atomic_store(&s_capture_restore_pending, false);
    } else if (error != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "重试恢复本地唤醒监听失败：%s", esp_err_to_name(error));
        atomic_store(&s_capture_restore_pending, false);
    }
}

/**
 * @brief 停止 AI 会话并恢复唤醒监听。
 *
 * 该函数不等待当前网络响应，先关闭本地会话状态和功放，再发送 cancel，
 * 从而保证后台晚到的音频帧不会再次打开设备扬声器。
 */
static void voice_ai_finish_session(const char *reason, bool notify_backend)
{
    bool was_active = atomic_exchange(&s_session_active, false);
    bool was_playing = atomic_exchange(&s_playing, false);
    atomic_store(&s_failure, VOICE_AI_FAILURE_NONE);

    if (was_active && notify_backend) {
        (void)voice_ai_send_event("cancel");
    }

    voice_service_stop_conversation();
    (void)voice_board_audio_set_output_enabled(false);
    voice_ai_clear_queue(s_audio_queue);
    voice_ai_clear_queue(s_control_queue);
    s_session_deadline_ms = 0;

    /* 播放过程中结束会话时，audio_start 已经暂停了采集任务，必须显式恢复唤醒监听。 */
    if (was_playing) {
        esp_err_t error = voice_service_start();
        if (error == ESP_OK) {
            atomic_store(&s_capture_restore_pending, false);
        } else if (error == ESP_ERR_INVALID_STATE) {
            /* 语音任务仍在退出时，等待任务完成后再启动。 */
            atomic_store(&s_capture_restore_pending, true);
        } else {
            ESP_LOGE(TAG, "AI 会话结束后恢复唤醒监听失败：%s", esp_err_to_name(error));
        }
    }

    if (reason != NULL && reason[0] != '\0') {
        ESP_LOGI(TAG, "AI 会话结束：%s", reason);
    }
}

/**
 * @brief 校验后台下发的音频输出配置。
 *
 * 当前设备播放链路只接收裸 PCM，因此明确拒绝 Opus/G711 等编码，避免把
 * 压缩码流错误地当作 int16 PCM 写入 ES8311。后台默认配置已经与这里一致。
 */
static bool voice_ai_parse_ready(cJSON *root)
{
    const char *provider = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "provider"));
    cJSON *output = cJSON_GetObjectItemCaseSensitive(root, "output");
    const char *codec = output == NULL
        ? NULL
        : cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(output, "codec"));
    cJSON *sample_rate = output == NULL
        ? NULL
        : cJSON_GetObjectItemCaseSensitive(output, "sampleRate");
    cJSON *channel = output == NULL
        ? NULL
        : cJSON_GetObjectItemCaseSensitive(output, "channel");
    cJSON *bit_depth = output == NULL
        ? NULL
        : cJSON_GetObjectItemCaseSensitive(output, "bitDepth");

    bool supported = provider != NULL && strcmp(provider, "coze_realtime") == 0 &&
                     codec != NULL && strcmp(codec, "pcm") == 0 &&
                     cJSON_IsNumber(sample_rate) && sample_rate->valueint == 16000 &&
                     cJSON_IsNumber(channel) && channel->valueint == 1 &&
                     cJSON_IsNumber(bit_depth) && bit_depth->valueint == 16;

    cloud_gateway_status_t gateway_status = {0};
    bool current_connection = cloud_gateway_get_status(&gateway_status) == ESP_OK &&
                              gateway_status.state == CLOUD_GATEWAY_READY;
    if (current_connection) {
        atomic_store(&s_ready_epoch, gateway_status.connection_epoch);
    }

    atomic_store(&s_supported_audio, supported && current_connection);
    atomic_store(&s_ready, supported && current_connection);
    if (!supported) {
        ESP_LOGE(TAG, "后台返回的 AI 音频配置不支持，要求 Coze PCM/16000Hz/单声道/16bit");
        return false;
    }

    ESP_LOGI(TAG, "后台 AI 语音配置已就绪：Coze PCM/16000Hz/单声道/16bit");
    return true;
}

/**
 * @brief 处理后台发来的文字事件。
 */
static void voice_ai_process_text(const uint8_t *data, size_t length)
{
    cJSON *root = cJSON_ParseWithLength((const char *)data, length);
    if (root == NULL) {
        ESP_LOGE(TAG, "后台 AI 文字事件不是有效 JSON");
        return;
    }

    const char *type = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "type"));
    if (type == NULL) {
        cJSON_Delete(root);
        return;
    }

    if (strcmp(type, "ready") == 0) {
        (void)voice_ai_parse_ready(root);
    } else if (strcmp(type, "audio_start") == 0) {
        if (atomic_load(&s_session_active) && atomic_load(&s_supported_audio)) {
            /* 先停止麦克风读取任务，再打开功放，形成确定的半双工边界。 */
            esp_err_t error = voice_service_stop();
            if (error != ESP_OK) {
                ESP_LOGE(TAG, "AI 播放前停止麦克风失败：%s", esp_err_to_name(error));
                atomic_store(&s_capture_restore_pending, true);
                voice_ai_finish_session("播放前停止采集失败", true);
            } else if (voice_board_audio_set_output_enabled(true) == ESP_OK) {
                atomic_store(&s_playing, true);
            } else {
                voice_ai_finish_session("打开扬声器失败", true);
                (void)voice_service_start();
            }
        }
    } else if (strcmp(type, "audio_end") == 0 || strcmp(type, "audio_stop") == 0) {
        if (atomic_load(&s_playing)) {
            voice_ai_resume_capture();
            /* 每次 AI 回复结束后重新给用户一段连续对话时间。 */
            s_session_deadline_ms = voice_ai_now_ms() + VOICE_AI_SESSION_TIMEOUT_MS;
        }
    } else if (strcmp(type, "error") == 0) {
        const char *message = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "message"));
        ESP_LOGE(TAG, "后台 AI 会话错误：%s", message != NULL ? message : "未提供错误信息");
        voice_ai_finish_session("后台返回错误", false);
    }

    cJSON_Delete(root);
}

/**
 * @brief AI 音频处理任务。
 *
 * 所有后台下行数据都在本任务中消费，避免 cloud_gateway 网络回调阻塞网络线程。
 */
static void voice_ai_worker(void *argument)
{
    (void)argument;

    for (;;) {
        voice_ai_message_t message = {0};
        /* 控制事件优先处理，确保 audio_start 先于已经到达的 PCM 音频。 */
        if (xQueueReceive(s_control_queue, &message, 0) == pdTRUE) {
            voice_ai_process_text(message.data, message.length);
            voice_ai_free_message(&message);
        } else if (xQueueReceive(s_audio_queue, &message, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (atomic_load(&s_session_active) && atomic_load(&s_playing) &&
                message.length > 0 && message.length % sizeof(int16_t) == 0) {
                esp_err_t error = voice_board_audio_write(
                    (const int16_t *)message.data,
                    message.length / sizeof(int16_t));
                if (error != ESP_OK) {
                    ESP_LOGE(TAG, "写入 AI PCM 播放设备失败：%s", esp_err_to_name(error));
                    voice_ai_finish_session("播放 PCM 失败", true);
                }
            }
            voice_ai_free_message(&message);
        }

        voice_ai_failure_t failure = (voice_ai_failure_t)atomic_exchange(&s_failure, VOICE_AI_FAILURE_NONE);
        if (failure != VOICE_AI_FAILURE_NONE) {
            voice_ai_finish_session(voice_ai_failure_name(failure), true);
        }
        voice_ai_retry_capture_restore();
        if (atomic_load(&s_session_active) && s_session_deadline_ms > 0 &&
            voice_ai_now_ms() >= s_session_deadline_ms) {
            voice_ai_finish_session("会话超时", true);
        }
    }
}

esp_err_t voice_ai_service_init(void)
{
    if (atomic_load(&s_initialized)) {
        return ESP_ERR_INVALID_STATE;
    }

    s_control_queue = xQueueCreate(VOICE_AI_CONTROL_QUEUE_LENGTH, sizeof(voice_ai_message_t));
    s_audio_queue = xQueueCreate(VOICE_AI_AUDIO_QUEUE_LENGTH, sizeof(voice_ai_message_t));
    if (s_control_queue == NULL || s_audio_queue == NULL) {
        if (s_control_queue != NULL) {
            vQueueDelete(s_control_queue);
            s_control_queue = NULL;
        }
        if (s_audio_queue != NULL) {
            vQueueDelete(s_audio_queue);
            s_audio_queue = NULL;
        }
        return ESP_ERR_NO_MEM;
    }

    atomic_store(&s_session_active, false);
    atomic_store(&s_playing, false);
    atomic_store(&s_ready, false);
    atomic_store(&s_supported_audio, false);
    atomic_store(&s_failure, VOICE_AI_FAILURE_NONE);
    atomic_store(&s_ready_epoch, 0);
    atomic_store(&s_capture_restore_pending, false);
    atomic_store(&s_ignored_event_count, 0);
    s_session_deadline_ms = 0;

    if (xTaskCreate(voice_ai_worker, "voice_ai", 8192, NULL, 5, NULL) != pdPASS) {
        vQueueDelete(s_control_queue);
        vQueueDelete(s_audio_queue);
        s_control_queue = NULL;
        s_audio_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    atomic_store(&s_initialized, true);
    return ESP_OK;
}

void voice_ai_service_on_wake_detected(void)
{
    if (!atomic_load(&s_initialized) || atomic_load(&s_session_active)) {
        return;
    }

    cloud_gateway_status_t status = {0};
    if (cloud_gateway_get_status(&status) != ESP_OK || status.state != CLOUD_GATEWAY_READY) {
        ESP_LOGW(TAG, "检测到唤醒词，但后台尚未就绪，恢复本地唤醒监听");
        voice_service_stop_conversation();
        return;
    }
    if (!atomic_load(&s_ready) || !atomic_load(&s_supported_audio) ||
        status.connection_epoch != atomic_load(&s_ready_epoch)) {
        ESP_LOGE(TAG, "后台尚未下发可用的 Coze PCM 音频配置，无法开始 AI 会话");
        voice_service_stop_conversation();
        return;
    }

    atomic_store(&s_session_active, true);
    s_session_deadline_ms = voice_ai_now_ms() + VOICE_AI_SESSION_TIMEOUT_MS;
    esp_err_t error = voice_ai_send_event("audio_start");
    if (error != ESP_OK) {
        voice_ai_finish_session("发送 AI 会话开始事件失败", false);
        return;
    }

    ESP_LOGI(TAG, "唤醒成功，已开始向后台上传 AI 对话音频");
}

void voice_ai_service_on_pcm(const int16_t *samples, size_t sample_count, void *context)
{
    (void)context;

    if (!atomic_load(&s_session_active) || atomic_load(&s_playing) ||
        samples == NULL || sample_count == 0) {
        return;
    }

    size_t length = sample_count * sizeof(int16_t);
    if (length > CLOUD_GATEWAY_MAX_MESSAGE) {
        voice_ai_set_failure(VOICE_AI_FAILURE_UPLINK_INVALID_ARGUMENT, "PCM 分片超过网关限制");
        return;
    }

    esp_err_t error = cloud_gateway_send(true, samples, length);
    if (error != ESP_OK) {
        voice_ai_failure_t failure = error == ESP_ERR_INVALID_STATE
            ? VOICE_AI_FAILURE_UPLINK_NOT_READY
            : error == ESP_ERR_TIMEOUT
                ? VOICE_AI_FAILURE_UPLINK_QUEUE_FULL
                : error == ESP_ERR_INVALID_ARG
                    ? VOICE_AI_FAILURE_UPLINK_INVALID_ARGUMENT
                    : VOICE_AI_FAILURE_UPLINK_OTHER;
        voice_ai_set_failure(failure, esp_err_to_name(error));
    }
}

void voice_ai_service_on_cloud_message(bool binary, const void *data, size_t length)
{
    if (!atomic_load(&s_initialized) || data == NULL || length == 0) {
        return;
    }

    /* 二进制只允许进入 PCM 队列；未开始会话时直接丢弃后台残留音频。 */
    if (binary && !atomic_load(&s_session_active)) {
        return;
    }

    /* 文本只接收设备协议控制事件，Coze 原始事件和增量文字直接丢弃。 */
    if (!binary && !voice_ai_is_control_message(data, length)) {
        unsigned ignored = atomic_fetch_add(&s_ignored_event_count, 1) + 1;
        if (ignored == 1 || ignored % 100 == 0) {
            ESP_LOGW(TAG, "已丢弃非设备协议事件：count=%u", ignored);
        }
        return;
    }

    if (length > (binary ? VOICE_AI_MAX_BINARY_MESSAGE : VOICE_AI_MAX_JSON_MESSAGE)) {
        voice_ai_set_failure(binary ? VOICE_AI_FAILURE_DOWNLINK_AUDIO_TOO_LARGE
                                     : VOICE_AI_FAILURE_DOWNLINK_CONTROL_TOO_LARGE,
                             "消息长度超出设备限制");
        return;
    }

    voice_ai_message_t message = {
        .binary = binary,
        .length = length,
        .data = malloc(length),
    };
    if (message.data == NULL) {
        voice_ai_set_failure(binary ? VOICE_AI_FAILURE_DOWNLINK_AUDIO_ALLOC
                                     : VOICE_AI_FAILURE_DOWNLINK_CONTROL_ALLOC,
                             "malloc 返回空指针");
        return;
    }
    memcpy(message.data, data, length);
    QueueHandle_t queue = binary ? s_audio_queue : s_control_queue;
    if (xQueueSend(queue, &message, 0) != pdTRUE) {
        voice_ai_free_message(&message);
        voice_ai_set_failure(binary ? VOICE_AI_FAILURE_DOWNLINK_AUDIO_QUEUE_FULL
                                     : VOICE_AI_FAILURE_DOWNLINK_CONTROL_QUEUE_FULL,
                             binary ? "PCM 播放速度低于后台下发速度" : "控制事件处理速度低于后台下发速度");
    }
}

void voice_ai_service_stop_session(const char *reason)
{
    voice_ai_finish_session(reason != NULL ? reason : "外部请求", true);
}

bool voice_ai_service_is_session_active(void)
{
    return atomic_load(&s_session_active);
}
