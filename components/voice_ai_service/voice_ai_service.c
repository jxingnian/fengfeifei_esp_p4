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
#include "encoder/impl/esp_opus_enc.h"
#include "decoder/impl/esp_opus_dec.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "voice_board_audio.h"
#include "voice_service.h"

/* 单条压缩音频限制为 4KB；控制 JSON 允许 16KB。 */
#define VOICE_AI_MAX_BINARY_MESSAGE 4096U
#define VOICE_AI_MAX_JSON_MESSAGE 16384U
#define VOICE_AI_CONTROL_QUEUE_LENGTH 32U
#define VOICE_AI_AUDIO_QUEUE_LENGTH 96U
#define VOICE_AI_OPUS_FRAME_MS 60U
#define VOICE_AI_INPUT_FRAME_SAMPLES (16000U * VOICE_AI_OPUS_FRAME_MS / 1000U)
#define VOICE_AI_INPUT_FRAME_BYTES (VOICE_AI_INPUT_FRAME_SAMPLES * sizeof(int16_t))
#define VOICE_AI_MAX_OPUS_PACKET 4096U
#define VOICE_AI_MAX_DECODED_BYTES 8192U
#define VOICE_AI_ENCODE_QUEUE_LENGTH 4U
/* 小智 opus_codec 使用 24KB；本工程 VOIP/complexity=5 为编码预留 32KB。 */
#define VOICE_AI_ENCODER_STACK_BYTES (32U * 1024U)
#define VOICE_AI_WORKER_STACK_BYTES (24U * 1024U)
#define VOICE_AI_SESSION_TIMEOUT_MS 120000LL

static const char *TAG = "voice_ai_service";

typedef struct {
    bool binary;
    bool audio_barrier;
    uint64_t sequence;
    size_t length;
    uint8_t *data;
} voice_ai_message_t;

typedef struct {
    unsigned generation;
    int16_t samples[VOICE_AI_INPUT_FRAME_SAMPLES];
} voice_ai_pcm_frame_t;

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
static QueueHandle_t s_encode_queue;
static SemaphoreHandle_t s_encoder_mutex;
static SemaphoreHandle_t s_capture_mutex;
static atomic_uint s_capture_generation;
static uint64_t s_message_sequence;
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
static void *s_opus_encoder;
static void *s_opus_decoder;
static bool s_input_opus;
static bool s_output_opus;
static size_t s_pcm_accumulated;
static voice_ai_pcm_frame_t s_capture_frame;
/* 采集拼帧由 capture mutex 保护；只有编码任务使用编码缓冲，解码缓冲只由 worker 使用。 */
static _Alignas(16) uint8_t s_encoded_buffer[VOICE_AI_MAX_OPUS_PACKET];
static _Alignas(16) int16_t s_decoded_buffer[VOICE_AI_MAX_DECODED_BYTES / sizeof(int16_t)];
static atomic_uint s_produced_frames;
static atomic_uint s_produced_samples;
static atomic_uint s_encoded_frames;
static atomic_uint s_decoded_frames;
static atomic_uint s_audio_dropped;
static atomic_uint s_audio_bytes;
static atomic_uint s_encoder_stack_free;
static int64_t s_rate_log_ms;

/* 队列清理函数位于消息释放函数之前，先声明以保持编译器检查有效。 */
static void voice_ai_free_message(voice_ai_message_t *message);

/* 只锁住很短的拼帧/入队过程，不等待编解码；代号使已经出队的旧 PCM 失效。 */
static void voice_ai_reset_capture(void)
{
    xSemaphoreTake(s_capture_mutex, portMAX_DELAY);
    atomic_fetch_add(&s_capture_generation, 1);
    s_pcm_accumulated = 0;
    xQueueReset(s_encode_queue);
    xSemaphoreGive(s_capture_mutex);
}

static esp_opus_dec_frame_duration_t opus_dec_duration(double ms)
{
    if (ms == 2.5) return ESP_OPUS_DEC_FRAME_DURATION_2_5_MS;
    if (ms == 5) return ESP_OPUS_DEC_FRAME_DURATION_5_MS;
    if (ms == 10) return ESP_OPUS_DEC_FRAME_DURATION_10_MS;
    if (ms == 20) return ESP_OPUS_DEC_FRAME_DURATION_20_MS;
    if (ms == 40) return ESP_OPUS_DEC_FRAME_DURATION_40_MS;
    if (ms == 60) return ESP_OPUS_DEC_FRAME_DURATION_60_MS;
    if (ms == 80) return ESP_OPUS_DEC_FRAME_DURATION_80_MS;
    if (ms == 100) return ESP_OPUS_DEC_FRAME_DURATION_100_MS;
    if (ms == 120) return ESP_OPUS_DEC_FRAME_DURATION_120_MS;
    return ESP_OPUS_DEC_FRAME_DURATION_INVALID;
}

static bool voice_ai_open_codecs(bool input_opus, bool output_opus,
                                 uint32_t output_rate, uint32_t output_channel,
                                 double output_frame_ms)
{
    if (s_opus_encoder) { esp_opus_enc_close(s_opus_encoder); s_opus_encoder = NULL; }
    if (s_opus_decoder) { (void)esp_opus_dec_close(s_opus_decoder); s_opus_decoder = NULL; }
    s_input_opus = input_opus;
    s_output_opus = output_opus;
    if (input_opus) {
        esp_opus_enc_config_t cfg = ESP_OPUS_ENC_CONFIG_DEFAULT();
        cfg.sample_rate = 16000;
        cfg.channel = 1;
        cfg.bits_per_sample = 16;
        cfg.bitrate = 24000;
        cfg.frame_duration = ESP_OPUS_ENC_FRAME_DURATION_60_MS;
        cfg.application_mode = ESP_OPUS_ENC_APPLICATION_VOIP;
        cfg.complexity = 5;
        cfg.enable_vbr = true;
        if (esp_opus_enc_open(&cfg, sizeof(cfg), &s_opus_encoder) != ESP_AUDIO_ERR_OK) {
            s_opus_encoder = NULL;
            return false;
        }
        int input_bytes = 0, output_bytes = 0;
        if (esp_opus_enc_get_frame_size(s_opus_encoder, &input_bytes, &output_bytes) != ESP_AUDIO_ERR_OK ||
            input_bytes != VOICE_AI_INPUT_FRAME_BYTES || output_bytes > sizeof(s_encoded_buffer)) {
            esp_opus_enc_close(s_opus_encoder);
            s_opus_encoder = NULL;
            return false;
        }
    }
    if (output_opus) {
        esp_opus_dec_cfg_t cfg = ESP_OPUS_DEC_CONFIG_DEFAULT();
        cfg.sample_rate = output_rate;
        cfg.channel = output_channel;
        cfg.frame_duration = opus_dec_duration(output_frame_ms);
        cfg.self_delimited = false;
        if (esp_opus_dec_open(&cfg, sizeof(cfg), &s_opus_decoder) != ESP_AUDIO_ERR_OK) {
            if (s_opus_encoder) { esp_opus_enc_close(s_opus_encoder); s_opus_encoder = NULL; }
            s_opus_decoder = NULL;
            return false;
        }
    }
    return true;
}

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

    voice_service_stop_conversation();
    (void)voice_board_audio_set_output_enabled(false);
    voice_ai_clear_queue(s_audio_queue);
    voice_ai_clear_queue(s_control_queue);
    voice_ai_reset_capture();
    /* 等在途编码完成后再清网关队列，防止 cancel 后重新塞入旧音频。 */
    xSemaphoreTake(s_encoder_mutex, portMAX_DELAY);
    cloud_gateway_discard_audio();
    xSemaphoreGive(s_encoder_mutex);
    if (was_active && notify_backend) {
        (void)voice_ai_send_event("cancel");
    }
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
 * PCM 直接播放；Opus 先解码成 PCM 再写入 ES8311。
 * 当前硬件采集/播放固定为 16kHz/单声道/16bit，不静默播放其他采样率。
 */
static bool voice_ai_parse_ready(cJSON *root)
{
    const char *provider = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "provider"));
    cJSON *input = cJSON_GetObjectItemCaseSensitive(root, "input");
    cJSON *output = cJSON_GetObjectItemCaseSensitive(root, "output");
    const char *input_codec = input == NULL ? NULL :
        cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(input, "codec"));
    const char *output_codec = output == NULL ? NULL :
        cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(output, "codec"));
    cJSON *input_rate = input == NULL ? NULL : cJSON_GetObjectItemCaseSensitive(input, "sampleRate");
    cJSON *input_channel = input == NULL ? NULL : cJSON_GetObjectItemCaseSensitive(input, "channel");
    cJSON *input_bits = input == NULL ? NULL : cJSON_GetObjectItemCaseSensitive(input, "bitDepth");
    cJSON *output_rate = output == NULL ? NULL : cJSON_GetObjectItemCaseSensitive(output, "sampleRate");
    cJSON *output_channel = output == NULL ? NULL : cJSON_GetObjectItemCaseSensitive(output, "channel");
    cJSON *output_bits = output == NULL ? NULL : cJSON_GetObjectItemCaseSensitive(output, "bitDepth");
    cJSON *frame_size = output == NULL ? NULL : cJSON_GetObjectItemCaseSensitive(output, "frameSizeMs");
    double output_frame_ms = cJSON_IsNumber(frame_size) ? frame_size->valuedouble : 60.0;

    bool input_ok = input_codec != NULL &&
                    (!strcmp(input_codec, "pcm") || !strcmp(input_codec, "opus")) &&
                    cJSON_IsNumber(input_rate) && input_rate->valueint == 16000 &&
                    cJSON_IsNumber(input_channel) && input_channel->valueint == 1 &&
                    cJSON_IsNumber(input_bits) && input_bits->valueint == 16;
    bool output_ok = output_codec != NULL &&
                     (!strcmp(output_codec, "pcm") || !strcmp(output_codec, "opus")) &&
                     cJSON_IsNumber(output_rate) && output_rate->valueint == 16000 &&
                     cJSON_IsNumber(output_channel) && output_channel->valueint == 1 &&
                     cJSON_IsNumber(output_bits) && output_bits->valueint == 16;
    bool supported = provider != NULL && strcmp(provider, "coze_realtime") == 0 &&
                     input_ok && output_ok &&
                     (strcmp(output_codec, "opus") ||
                      opus_dec_duration(output_frame_ms) != ESP_OPUS_DEC_FRAME_DURATION_INVALID);

    cloud_gateway_status_t gateway_status = {0};
    bool current_connection = cloud_gateway_get_status(&gateway_status) == ESP_OK &&
                              gateway_status.state == CLOUD_GATEWAY_READY;
    if (atomic_load(&s_session_active) && current_connection &&
        gateway_status.connection_epoch != atomic_load(&s_ready_epoch)) {
        voice_ai_finish_session("后台连接已更换，重新等待唤醒", false);
    }
    if (current_connection) {
        atomic_store(&s_ready_epoch, gateway_status.connection_epoch);
    }

    atomic_store(&s_ready, false);
    voice_ai_reset_capture();
    xSemaphoreTake(s_encoder_mutex, portMAX_DELAY);
    bool codecs_ready = supported && voice_ai_open_codecs(
        !strcmp(input_codec, "opus"), !strcmp(output_codec, "opus"),
        (uint32_t)output_rate->valueint, (uint32_t)output_channel->valueint,
        output_frame_ms);
    xSemaphoreGive(s_encoder_mutex);
    atomic_store(&s_supported_audio, codecs_ready && current_connection);
    atomic_store(&s_ready, codecs_ready && current_connection);
    if (!supported || !codecs_ready) {
        ESP_LOGE(TAG, "后台 AI 音频配置不支持或 Opus 初始化失败：输入=%s，输出=%s，要求 16kHz/单声道/16bit",
                 input_codec != NULL ? input_codec : "?", output_codec != NULL ? output_codec : "?");
        return false;
    }

    ESP_LOGI(TAG, "后台 AI 语音配置已就绪：输入=%s，输出=%s，16000Hz/单声道/16bit，输出帧=%.1fms",
             input_codec, output_codec, output_frame_ms);
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
                voice_ai_reset_capture();
                xSemaphoreTake(s_encoder_mutex, portMAX_DELAY);
                cloud_gateway_discard_audio();
                xSemaphoreGive(s_encoder_mutex);
                atomic_store(&s_playing, true);
            } else {
                voice_ai_finish_session("打开扬声器失败", true);
                (void)voice_service_start();
            }
        }
    } else if (strcmp(type, "audio_end") == 0 || strcmp(type, "audio_stop") == 0) {
        if (strcmp(type, "audio_stop") == 0) voice_ai_clear_queue(s_audio_queue);
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

static bool voice_ai_pcm_frame_is_current(const voice_ai_pcm_frame_t *frame)
{
    return frame->generation == atomic_load(&s_capture_generation) &&
           atomic_load(&s_session_active) && atomic_load(&s_ready) &&
           !atomic_load(&s_playing);
}

/* Opus/SILK 内部有大栈帧，只能在本任务中编码，不能借用麦克风任务的 8KB 栈。 */
static void voice_ai_encode_worker(void *argument)
{
    (void)argument;
    voice_ai_pcm_frame_t frame;
    bool logged_stack = false;
    for (;;) {
        if (xQueueReceive(s_encode_queue, &frame, portMAX_DELAY) != pdTRUE) continue;
        xSemaphoreTake(s_encoder_mutex, portMAX_DELAY);
        if (!voice_ai_pcm_frame_is_current(&frame)) {
            xSemaphoreGive(s_encoder_mutex);
            continue;
        }

        uint8_t *data = (uint8_t *)frame.samples;
        size_t length = sizeof(frame.samples);
        esp_audio_err_t codec_error = ESP_AUDIO_ERR_OK;
        if (s_input_opus) {
            esp_audio_enc_in_frame_t in = {
                .buffer = data, .len = (uint32_t)length,
            };
            esp_audio_enc_out_frame_t out = {
                .buffer = s_encoded_buffer, .len = sizeof(s_encoded_buffer),
            };
            codec_error = s_opus_encoder
                ? esp_opus_enc_process(s_opus_encoder, &in, &out)
                : ESP_AUDIO_ERR_INVALID_PARAMETER;
            if (codec_error == ESP_AUDIO_ERR_OK &&
                (out.encoded_bytes == 0 || out.encoded_bytes > sizeof(s_encoded_buffer))) {
                codec_error = ESP_AUDIO_ERR_FAIL;
            }
            if (codec_error == ESP_AUDIO_ERR_OK) {
                data = s_encoded_buffer;
                length = out.encoded_bytes;
                atomic_fetch_add(&s_encoded_frames, 1);
            }
        }

        esp_err_t send_error = ESP_OK;
        if (codec_error != ESP_AUDIO_ERR_OK) {
            atomic_fetch_add(&s_audio_dropped, 1);
        } else if (voice_ai_pcm_frame_is_current(&frame)) {
            /* 编码时会话可能已切换；再次校验代号，旧帧不能进入新会话。 */
            atomic_fetch_add(&s_audio_bytes, (unsigned)length);
            send_error = cloud_gateway_send(true, data, length);
            if (send_error != ESP_OK) atomic_fetch_add(&s_audio_dropped, 1);
        }
        xSemaphoreGive(s_encoder_mutex);

        unsigned stack_free = (unsigned)uxTaskGetStackHighWaterMark(NULL);
        atomic_store(&s_encoder_stack_free, stack_free);
        if (!logged_stack) {
            ESP_LOGI(TAG, "AI 编码任务首帧完成：栈=%u bytes，最低剩余=%u bytes",
                     VOICE_AI_ENCODER_STACK_BYTES, stack_free);
            logged_stack = true;
        }
        if (codec_error != ESP_AUDIO_ERR_OK) {
            ESP_LOGW(TAG, "AI PCM→Opus 编码失败，已丢弃一帧：ret=%d", (int)codec_error);
        } else if (send_error != ESP_OK && send_error != ESP_ERR_TIMEOUT) {
            ESP_LOGW(TAG, "AI 上行音频包未入队，已丢弃：%s", esp_err_to_name(send_error));
        }
    }
}

/**
 * @brief AI 控制、下行解码和播放任务。
 *
 * 所有后台下行数据都在本任务中消费，避免 cloud_gateway 网络回调阻塞网络线程。
 */
static void voice_ai_worker(void *argument)
{
    (void)argument;

    for (;;) {
        voice_ai_message_t message = {0};
        /* 控制优先，但 audio_end 必须等待此前入队的音频播完。 */
        voice_ai_message_t control_head, audio_head;
        bool control_pending = xQueuePeek(s_control_queue, &control_head, 0) == pdTRUE;
        bool drain_audio = control_pending && control_head.audio_barrier &&
                           xQueuePeek(s_audio_queue, &audio_head, 0) == pdTRUE &&
                           audio_head.sequence < control_head.sequence;
        if (control_pending && !drain_audio && xQueueReceive(s_control_queue, &message, 0) == pdTRUE) {
            voice_ai_process_text(message.data, message.length);
            voice_ai_free_message(&message);
        } else if (xQueueReceive(s_audio_queue, &message, pdMS_TO_TICKS(20)) == pdTRUE) {
            /* 等待音频期间可能先后到达 audio_start 和首帧，先补处理控制，
             * 但不能让结束屏障越过当前已经出队、尚未播放的这一帧。 */
            while (xQueuePeek(s_control_queue, &control_head, 0) == pdTRUE &&
                   (!control_head.audio_barrier || control_head.sequence < message.sequence)) {
                voice_ai_message_t control;
                if (xQueueReceive(s_control_queue, &control, 0) != pdTRUE) break;
                voice_ai_process_text(control.data, control.length);
                voice_ai_free_message(&control);
            }
            if (atomic_load(&s_session_active) && atomic_load(&s_playing) &&
                message.length > 0) {
                esp_err_t error = ESP_OK;
                if (!s_output_opus) {
                    if (message.length % sizeof(int16_t) != 0) {
                        error = ESP_ERR_INVALID_SIZE;
                    } else {
                        error = voice_board_audio_write((const int16_t *)message.data,
                                                        message.length / sizeof(int16_t));
                    }
                } else if (s_opus_decoder) {
                    uint8_t *pcm = (uint8_t *)s_decoded_buffer;
                    esp_audio_dec_in_raw_t raw = {
                        .buffer = message.data, .len = (uint32_t)message.length,
                    };
                    while (raw.len > 0) {
                        esp_audio_dec_out_frame_t out = {
                            .buffer = pcm, .len = sizeof(s_decoded_buffer),
                        };
                        esp_audio_dec_info_t decode_info = {0};
                        esp_audio_err_t decode_error = esp_opus_dec_decode(
                            s_opus_decoder, &raw, &out, &decode_info);
                        if (decode_error != ESP_AUDIO_ERR_OK || out.decoded_size == 0 ||
                            out.decoded_size % sizeof(int16_t) != 0 ||
                            out.decoded_size > sizeof(s_decoded_buffer) ||
                            raw.consumed == 0 || raw.consumed > raw.len) {
                            error = ESP_ERR_INVALID_RESPONSE;
                            ESP_LOGW(TAG, "AI Opus 下行解码失败：ret=%d，length=%u，consumed=%u",
                                     (int)decode_error, (unsigned)message.length,
                                     (unsigned)raw.consumed);
                            break;
                        }
                        error = voice_board_audio_write((const int16_t *)pcm,
                                                        out.decoded_size / sizeof(int16_t));
                        atomic_fetch_add(&s_decoded_frames, 1);
                        if (error != ESP_OK) break;
                        raw.buffer += raw.consumed;
                        raw.len -= raw.consumed;
                    }
                } else {
                    error = ESP_ERR_INVALID_STATE;
                }
                if (error != ESP_OK) {
                    atomic_fetch_add(&s_audio_dropped, 1);
                    /* 单个音频包失败只丢包，不能因一个坏包结束整轮会话。 */
                    ESP_LOGW(TAG, "AI 下行音频包处理失败，已丢弃：%s", esp_err_to_name(error));
                }
            }
            voice_ai_free_message(&message);
        }

        int64_t now = voice_ai_now_ms();
        if (s_rate_log_ms == 0) s_rate_log_ms = now;
        if (now - s_rate_log_ms >= 5000) {
            unsigned frames = atomic_exchange(&s_produced_frames, 0);
            unsigned samples = atomic_exchange(&s_produced_samples, 0);
            unsigned encoded = atomic_exchange(&s_encoded_frames, 0);
            unsigned bytes = atomic_exchange(&s_audio_bytes, 0);
            unsigned decoded = atomic_exchange(&s_decoded_frames, 0);
            unsigned dropped = atomic_exchange(&s_audio_dropped, 0);
            double seconds = (double)(now - s_rate_log_ms) / 1000.0;
            if (frames || encoded || decoded || dropped) {
                ESP_LOGI(TAG, "AI 音频生产：窗口=%ums，PCM=%u samples（%.2f KiB/s），采集=%u，编码=%u 帧（%.2f 帧/s），待发=%u bytes（%.2f KiB/s），解码=%u，丢弃=%u，播放队列=%u/%u",
                         (unsigned)(now - s_rate_log_ms), samples, samples * sizeof(int16_t) / seconds / 1024.0,
                         frames, encoded, encoded / seconds, bytes, bytes / seconds / 1024.0,
                         decoded, dropped, (unsigned)uxQueueMessagesWaiting(s_audio_queue),
                         VOICE_AI_AUDIO_QUEUE_LENGTH);
                ESP_LOGI(TAG, "AI 任务栈：编码最低剩余=%u/%u bytes，解码/控制最低剩余=%u/%u bytes，PCM编码队列=%u/%u",
                         (unsigned)atomic_load(&s_encoder_stack_free), VOICE_AI_ENCODER_STACK_BYTES,
                         (unsigned)uxTaskGetStackHighWaterMark(NULL), VOICE_AI_WORKER_STACK_BYTES,
                         (unsigned)uxQueueMessagesWaiting(s_encode_queue), VOICE_AI_ENCODE_QUEUE_LENGTH);
            }
            s_rate_log_ms = now;
        }

        voice_ai_failure_t failure = (voice_ai_failure_t)atomic_exchange(&s_failure, VOICE_AI_FAILURE_NONE);
        if (failure != VOICE_AI_FAILURE_NONE) {
            voice_ai_finish_session(voice_ai_failure_name(failure), true);
        }
        voice_ai_retry_capture_restore();
        if (atomic_load(&s_session_active)) {
            cloud_gateway_status_t status;
            if (cloud_gateway_get_status(&status) != ESP_OK || status.state != CLOUD_GATEWAY_READY ||
                status.connection_epoch != atomic_load(&s_ready_epoch)) {
                atomic_store(&s_ready, false);
                atomic_store(&s_supported_audio, false);
                voice_ai_finish_session("后台连接已断开", false);
            }
        }
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
    s_encode_queue = xQueueCreate(VOICE_AI_ENCODE_QUEUE_LENGTH, sizeof(voice_ai_pcm_frame_t));
    s_encoder_mutex = xSemaphoreCreateMutex();
    s_capture_mutex = xSemaphoreCreateMutex();
    TaskHandle_t encode_task = NULL;
    if (!s_control_queue || !s_audio_queue || !s_encode_queue || !s_encoder_mutex || !s_capture_mutex)
        goto no_memory;

    atomic_store(&s_session_active, false);
    atomic_store(&s_playing, false);
    atomic_store(&s_ready, false);
    atomic_store(&s_supported_audio, false);
    atomic_store(&s_failure, VOICE_AI_FAILURE_NONE);
    atomic_store(&s_ready_epoch, 0);
    atomic_store(&s_capture_restore_pending, false);
    atomic_store(&s_ignored_event_count, 0);
    atomic_store(&s_capture_generation, 0);
    atomic_store(&s_encoder_stack_free, 0);
    s_session_deadline_ms = 0;

    if (xTaskCreate(voice_ai_encode_worker, "voice_ai_enc", VOICE_AI_ENCODER_STACK_BYTES,
                    NULL, 4, &encode_task) != pdPASS) goto no_memory;
    if (xTaskCreate(voice_ai_worker, "voice_ai", VOICE_AI_WORKER_STACK_BYTES, NULL, 5, NULL) != pdPASS) {
        /* initialized 尚未发布，编码任务只会等待空队列，可以安全清理。 */
        vTaskDelete(encode_task);
        goto no_memory;
    }

    atomic_store(&s_initialized, true);
    ESP_LOGI(TAG, "AI 音频任务已创建：编码栈=%u bytes，解码/控制栈=%u bytes，PCM编码队列=%u",
             VOICE_AI_ENCODER_STACK_BYTES, VOICE_AI_WORKER_STACK_BYTES, VOICE_AI_ENCODE_QUEUE_LENGTH);
    return ESP_OK;

no_memory:
    if (s_encoder_mutex) vSemaphoreDelete(s_encoder_mutex);
    if (s_capture_mutex) vSemaphoreDelete(s_capture_mutex);
    if (s_control_queue) vQueueDelete(s_control_queue);
    if (s_audio_queue) vQueueDelete(s_audio_queue);
    if (s_encode_queue) vQueueDelete(s_encode_queue);
    s_encoder_mutex = s_capture_mutex = NULL;
    s_control_queue = s_audio_queue = s_encode_queue = NULL;
    return ESP_ERR_NO_MEM;
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
        ESP_LOGE(TAG, "后台尚未下发可用的 PCM/Opus 音频配置，无法开始 AI 会话");
        voice_service_stop_conversation();
        return;
    }

    voice_ai_reset_capture();
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

    if (!atomic_load(&s_initialized) || !atomic_load(&s_session_active) ||
        !atomic_load(&s_ready) || atomic_load(&s_playing) ||
        samples == NULL || sample_count == 0) {
        return;
    }

    atomic_fetch_add(&s_produced_frames, 1);
    atomic_fetch_add(&s_produced_samples, (unsigned)sample_count);
    /* 本回调不调用 Opus、不等网络，也不等待正在运行的编码任务。 */
    if (xSemaphoreTake(s_capture_mutex, 0) != pdTRUE) {
        atomic_fetch_add(&s_audio_dropped, 1);
        return;
    }
    if (!atomic_load(&s_session_active) || !atomic_load(&s_ready) || atomic_load(&s_playing)) {
        xSemaphoreGive(s_capture_mutex);
        return;
    }
    const uint8_t *source = (const uint8_t *)samples;
    size_t bytes = sample_count * sizeof(int16_t);
    while (bytes > 0) {
        size_t copy = VOICE_AI_INPUT_FRAME_BYTES - s_pcm_accumulated;
        if (copy > bytes) copy = bytes;
        memcpy((uint8_t *)s_capture_frame.samples + s_pcm_accumulated, source, copy);
        s_pcm_accumulated += copy;
        source += copy;
        bytes -= copy;
        if (s_pcm_accumulated != VOICE_AI_INPUT_FRAME_BYTES) continue;

        s_capture_frame.generation = atomic_load(&s_capture_generation);
        if (xQueueSend(s_encode_queue, &s_capture_frame, 0) != pdTRUE) {
            /* 与小智一致：编码忙时丢最旧 PCM，不能拖住麦克风读取。 */
            static voice_ai_pcm_frame_t discarded;
            if (xQueueReceive(s_encode_queue, &discarded, 0) == pdTRUE)
                atomic_fetch_add(&s_audio_dropped, 1);
            if (xQueueSend(s_encode_queue, &s_capture_frame, 0) != pdTRUE)
                atomic_fetch_add(&s_audio_dropped, 1);
        }
        s_pcm_accumulated = 0;
    }
    xSemaphoreGive(s_capture_mutex);
}

void voice_ai_service_on_cloud_message(bool binary, const void *data, size_t length)
{
    if (!atomic_load(&s_initialized) || data == NULL || length == 0) {
        return;
    }

    /* 二进制只允许进入音频队列；未开始会话时直接丢弃后台残留音频。 */
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
        if (binary) {
            atomic_fetch_add(&s_audio_dropped, 1);
            ESP_LOGW(TAG, "AI 音频消息过大，已丢弃：%u", (unsigned)length);
            return;
        }
        voice_ai_set_failure(VOICE_AI_FAILURE_DOWNLINK_CONTROL_TOO_LARGE,
                             "消息长度超出设备限制");
        return;
    }

    voice_ai_message_t message = {
        .binary = binary,
        .sequence = ++s_message_sequence,
        .length = length,
        .data = malloc(length),
    };
    bool urgent = false;
    if (!binary) {
        cJSON *root = cJSON_ParseWithLength(data, length);
        const char *type = root ? cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "type")) : NULL;
        message.audio_barrier = type && !strcmp(type, "audio_end");
        urgent = type && (!strcmp(type, "audio_stop") || !strcmp(type, "error"));
        cJSON_Delete(root);
    }
    if (message.data == NULL) {
        if (binary) {
            atomic_fetch_add(&s_audio_dropped, 1);
            return;
        }
        voice_ai_set_failure(VOICE_AI_FAILURE_DOWNLINK_CONTROL_ALLOC,
                             "malloc 返回空指针");
        return;
    }
    memcpy(message.data, data, length);
    QueueHandle_t queue = binary ? s_audio_queue : s_control_queue;
    BaseType_t queued = urgent ? xQueueSendToFront(queue, &message, 0) : xQueueSend(queue, &message, 0);
    if (queued != pdTRUE) {
        if (binary) {
            /* 播放是实时链路：队列满时淘汰最旧包，不能阻塞 WebSocket 回调。 */
            voice_ai_message_t old = {0};
            if (xQueueReceive(queue, &old, 0) == pdTRUE) {
                voice_ai_free_message(&old);
                atomic_fetch_add(&s_audio_dropped, 1);
            }
            if (xQueueSend(queue, &message, 0) == pdTRUE) return;
            atomic_fetch_add(&s_audio_dropped, 1);
        }
        voice_ai_free_message(&message);
        if (!binary) voice_ai_set_failure(VOICE_AI_FAILURE_DOWNLINK_CONTROL_QUEUE_FULL,
                                         "控制事件不可静默丢弃，需重置会话状态");
        ESP_LOGW(TAG, "AI %s队列已满，消息已丢弃",
                 binary ? "音频" : "控制");
    }
}

void voice_ai_service_stop_session(const char *reason)
{
    if (!atomic_load(&s_initialized)) return;
    voice_ai_finish_session(reason != NULL ? reason : "外部请求", true);
}

bool voice_ai_service_is_session_active(void)
{
    return atomic_load(&s_session_active);
}
