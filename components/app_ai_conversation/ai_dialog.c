/**
 * @file ai_dialog.c
 * @brief AI 对话应用层。
 *
 * 本文件只负责把唤醒服务、音频流水线和抽象 AI 后端连接起来。会话状态由
 * ai_dialog_logic 维护，Coze 协议由 coze_app 维护；本文件不
 * 直接包含任何生成组件的内部实现。
 */
#include "ai_dialog.h"
#include "coze_app.h"
#include "ai_dialog_logic.h"
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_vad.h"
#include "decoder/impl/esp_opus_dec.h"
#include "esp_audio_dec.h"
#include "encoder/impl/esp_opus_enc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "audio_service.h"
#include "wake_word.h"
#include "ai_turn.h"
#include "alert_drip_data.h"

#define AI_DIALOG_ENCODER_STACK_BYTES (32U * 1024U)
#define AI_DIALOG_WORKER_STACK_BYTES (24U * 1024U)
#define FRAME_SAMPLES 960U
#define FRAME_MS 60U
#define PRE_ROLL_FRAMES 5U
#define ENCODE_QUEUE_LENGTH 16U /* 960ms raw capture */
#define PCM_QUEUE_LENGTH 4U
#define MAX_PACKET 4096U
#define MAX_PCM_BYTES 8192U
#define CONTROL_QUEUE_LENGTH 32U
#define OPUS_RING_BUFFER_BYTES (200U * 1024U)
#define OPUS_RING_DESCRIPTOR_COUNT 4096U
#define ALERT_DRIP_PCM_PEAK 16113

/*
 * 播放打断检测已迁移到 audio_service 的统一 VAD，通过回调机制实现。
 * 不再需要独立的打断 VAD 实例和前置缓冲区。
 */

typedef struct { unsigned generation; int16_t samples[FRAME_SAMPLES]; } capture_t;
typedef struct { size_t length; uint8_t *data; unsigned turn, seq; } packet_t;
typedef struct { size_t length; uint8_t *data; unsigned generation, turn, seq; } pcm_t;
typedef struct {
    size_t offset;
    size_t length;
    unsigned turn;
    unsigned seq;
} opus_packet_desc_t;
static const char *TAG = "ai_dialog";

// 新架构：保存事件回调配置
static ai_dialog_config_t s_event_config;

static QueueHandle_t s_control_queue, s_encode_queue, s_pcm_queue;
static SemaphoreHandle_t s_control_mutex, s_encoder_mutex, s_pcm_mutex, s_output_mutex, s_capture_mutex;
static SemaphoreHandle_t s_opus_mutex, s_opus_data_sem;
static atomic_bool s_initialized, s_ready, s_wake_requested, s_restore_pending;
static atomic_bool s_interrupt_requested;
static atomic_bool s_interrupt_in_progress;
static atomic_uint s_generation, s_turn, s_failure;
static atomic_uint s_produced_samples, s_encoded, s_decoded, s_written, s_tx_bytes;
static atomic_uint s_capture_drops, s_encode_drops, s_rx_drops, s_decode_drops, s_write_drops;
static atomic_uint s_completed_seq, s_encoder_stack_free, s_pcm_stack_free, s_output_stack_free;
static atomic_bool s_end_seen;
static atomic_llong s_listen_since, s_wait_since, s_audio_since, s_playback_since;
static unsigned s_end_sequence;
static bool s_wait_warning, s_input_opus;
static capture_t s_capture_frame;
static size_t s_captured;
/* 播放打断检测已迁移到 audio_service 的统一 VAD。 */
static void *s_encoder;
static void *s_decoder;
static vad_handle_t s_vad;
static _Alignas(16) uint8_t s_encoded_buffer[MAX_PACKET];
static capture_t s_pre_roll[PRE_ROLL_FRAMES]; /* encoder owns ring */
static uint8_t *s_opus_ring;
static opus_packet_desc_t *s_opus_descriptors;
static size_t s_opus_write_offset;
static size_t s_opus_used_bytes;
static size_t s_opus_write_desc;
static size_t s_opus_read_desc;
static size_t s_opus_desc_count;
static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

/**
 * @brief 向状态机上报AI对话事件
 */
static void emit_ai_event(ai_dialog_event_type_t type, const char *message)
{
    if (s_event_config.on_event == NULL) {
        return;
    }

    ai_dialog_event_t event = {
        .type = type,
        .message = message,
    };
    s_event_config.on_event(&event, s_event_config.context);
}

static unsigned json_uint(cJSON *root, const char *key)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(root, key);
    return cJSON_IsNumber(v) && v->valuedouble >= 0 && v->valuedouble <= UINT32_MAX
        ? (unsigned)v->valuedouble : 0;
}
static const char *json_str(cJSON *root, const char *key)
{
    const char *s = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, key));
    return s ? s : "";
}
static bool playing(void)
{
    return ai_dialog_logic_get_state() == AI_DIALOG_LOGIC_STATE_PLAYING;
}
static bool current(unsigned generation, unsigned turn)
{
    return ai_dialog_logic_is_session_active() && generation == atomic_load(&s_generation) && turn == atomic_load(&s_turn);
}
/**
 * @brief 在环形缓冲区中复制一段数据，允许数据跨越物理尾部。
 *
 * Opus 每帧都必须保持完整边界，因此环形缓冲区只保存字节，实际帧边界
 * 由 s_opus_descriptors 记录。写入和读取都通过本函数处理回绕。
 */
static void opus_ring_copy(
    uint8_t *buffer,
    size_t offset,
    uint8_t *data,
    size_t length,
    bool write_to_ring
)
{
    size_t first = OPUS_RING_BUFFER_BYTES - offset;
    if (first > length) {
        first = length;
    }
    if (write_to_ring) {
        memcpy(buffer + offset, data, first);
        if (length > first) {
            memcpy(buffer, data + first, length - first);
        }
    } else {
        memcpy(data, buffer + offset, first);
        if (length > first) {
            memcpy(data + first, buffer, length - first);
        }
    }
}

/**
 * @brief 丢弃环形缓冲区中最旧的一帧 Opus。
 *
 * 服务端下发速度高于解码/播放速度时，必须按完整帧淘汰，不能只移动几个
 * 字节，否则后续 Opus 帧边界会损坏，解码器会连续报错。
 */
static void opus_ring_discard_oldest_locked(void)
{
    if (s_opus_desc_count == 0) {
        return;
    }
    const opus_packet_desc_t *descriptor = &s_opus_descriptors[s_opus_read_desc];
    s_opus_used_bytes -= descriptor->length;
    s_opus_read_desc = (s_opus_read_desc + 1U) % OPUS_RING_DESCRIPTOR_COUNT;
    s_opus_desc_count--;
}

/**
 * @brief 把一帧 Opus 复制到 PSRAM 环形缓冲区。
 *
 * 该函数运行在 Coze WebSocket 任务中，只允许零等待获取互斥锁；当缓冲区
 * 空间不足时淘汰最旧完整帧，确保 WebSocket 接收任务不会被播放阻塞。
 */
static bool opus_ring_push(
    const uint8_t *data,
    size_t length,
    unsigned turn,
    unsigned sequence,
    unsigned *evicted_count
)
{
    if (evicted_count != NULL) {
        *evicted_count = 0;
    }
    if (s_opus_ring == NULL || s_opus_descriptors == NULL ||
        s_opus_mutex == NULL || data == NULL || length == 0 ||
        length > OPUS_RING_BUFFER_BYTES || length > MAX_PACKET) {
        return false;
    }
    if (xSemaphoreTake(s_opus_mutex, 0) != pdTRUE) {
        return false;
    }

    unsigned evicted = 0;
    while (s_opus_desc_count >= OPUS_RING_DESCRIPTOR_COUNT ||
           s_opus_used_bytes + length > OPUS_RING_BUFFER_BYTES) {
        if (s_opus_desc_count == 0) {
            xSemaphoreGive(s_opus_mutex);
            return false;
        }
        opus_ring_discard_oldest_locked();
        evicted++;
    }

    const size_t write_offset = s_opus_write_offset;
    opus_ring_copy(s_opus_ring, write_offset, (uint8_t *)data, length, true);
    s_opus_descriptors[s_opus_write_desc] = (opus_packet_desc_t){
        .offset = write_offset,
        .length = length,
        .turn = turn,
        .seq = sequence,
    };
    s_opus_write_offset = (write_offset + length) % OPUS_RING_BUFFER_BYTES;
    s_opus_used_bytes += length;
    s_opus_write_desc = (s_opus_write_desc + 1U) % OPUS_RING_DESCRIPTOR_COUNT;
    s_opus_desc_count++;
    xSemaphoreGive(s_opus_mutex);
    if (evicted_count != NULL) {
        *evicted_count = evicted;
    }
    xSemaphoreGive(s_opus_data_sem);
    return true;
}

/**
 * @brief 从 PSRAM 环形缓冲区取出一帧完整 Opus 数据。
 *
 * 取出时立即释放环形缓冲区空间，解码过程在互斥锁外执行，避免解码耗时
 * 阻塞 Coze 回调继续接收后续帧。
 */
static bool opus_ring_pop(
    uint8_t *data,
    size_t capacity,
    size_t *length,
    unsigned *turn,
    unsigned *sequence
)
{
    if (data == NULL || length == NULL || turn == NULL || sequence == NULL ||
        s_opus_mutex == NULL || s_opus_ring == NULL || s_opus_descriptors == NULL ||
        xSemaphoreTake(s_opus_mutex, 0) != pdTRUE) {
        return false;
    }
    if (s_opus_desc_count == 0) {
        xSemaphoreGive(s_opus_mutex);
        return false;
    }

    const opus_packet_desc_t descriptor = s_opus_descriptors[s_opus_read_desc];
    if (descriptor.length > capacity) {
        opus_ring_discard_oldest_locked();
        xSemaphoreGive(s_opus_mutex);
        return false;
    }
    opus_ring_copy(s_opus_ring, descriptor.offset, data, descriptor.length, false);
    *length = descriptor.length;
    *turn = descriptor.turn;
    *sequence = descriptor.seq;
    s_opus_used_bytes -= descriptor.length;
    s_opus_read_desc = (s_opus_read_desc + 1U) % OPUS_RING_DESCRIPTOR_COUNT;
    s_opus_desc_count--;
    xSemaphoreGive(s_opus_mutex);
    return true;
}

/**
 * @brief 清空 Opus 环形缓冲区和帧描述符。
 */
static void opus_ring_clear(void)
{
    if (s_opus_mutex == NULL || xSemaphoreTake(s_opus_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }
    s_opus_write_offset = 0;
    s_opus_used_bytes = 0;
    s_opus_write_desc = 0;
    s_opus_read_desc = 0;
    s_opus_desc_count = 0;
    xSemaphoreGive(s_opus_mutex);
}

#if CONFIG_AI_DIALOG_VERBOSE_STATS
/**
 * @brief 读取统计用的 Opus 缓冲区当前占用字节数。
 */
static size_t opus_ring_get_used_bytes(void)
{
    size_t used = 0;
    if (s_opus_mutex != NULL && xSemaphoreTake(s_opus_mutex, 0) == pdTRUE) {
        used = s_opus_used_bytes;
        xSemaphoreGive(s_opus_mutex);
    }
    return used;
}

/**
 * @brief 读取统计用的 Opus 待解码帧数量。
 */
static size_t opus_ring_get_packet_count(void)
{
    size_t count = 0;
    if (s_opus_mutex != NULL && xSemaphoreTake(s_opus_mutex, 0) == pdTRUE) {
        count = s_opus_desc_count;
        xSemaphoreGive(s_opus_mutex);
    }
    return count;
}
#endif

/**
 * @brief 初始化 200 KB PSRAM Opus 环形缓冲区。
 */
static esp_err_t opus_ring_init(void)
{
    s_opus_ring = heap_caps_malloc(
        OPUS_RING_BUFFER_BYTES,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );
    s_opus_descriptors = heap_caps_calloc(
        OPUS_RING_DESCRIPTOR_COUNT,
        sizeof(opus_packet_desc_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );
    if (s_opus_ring == NULL || s_opus_descriptors == NULL) {
        free(s_opus_ring);
        free(s_opus_descriptors);
        s_opus_ring = NULL;
        s_opus_descriptors = NULL;
        return ESP_ERR_NO_MEM;
    }
    opus_ring_clear();
    ESP_LOGI(TAG, "下行 Opus PSRAM 缓冲区已创建：%u 字节，描述符：%u 帧",
             (unsigned)OPUS_RING_BUFFER_BYTES,
             (unsigned)OPUS_RING_DESCRIPTOR_COUNT);
    return ESP_OK;
}

/**
 * @brief 释放 Opus 环形缓冲区资源。
 */
static void opus_ring_deinit(void)
{
    free(s_opus_ring);
    free(s_opus_descriptors);
    s_opus_ring = NULL;
    s_opus_descriptors = NULL;
}

static void clear_pcm(void)
{
    pcm_t p;
    while (xQueueReceive(s_pcm_queue, &p, 0) == pdTRUE) free(p.data);
}
static void reset_capture(void)
{
    xSemaphoreTake(s_capture_mutex, portMAX_DELAY);
    atomic_fetch_add(&s_generation, 1); s_captured = 0;
    xQueueReset(s_encode_queue);
    xSemaphoreGive(s_capture_mutex);
}

/**
 * @brief 清零播放期间的本地打断检测器。
 *
 * 该函数只清除检测计数，不清除前置 PCM；前置 PCM 会在真正开始新 turn
 * 时重新送入编码队列，保证用户第一句话的开头不会丢失。
 */
/**
 * @brief VAD 状态变化回调函数。
 *
 * 当 audio_service 检测到 VAD 状态变化时调用。
 * 播放期间检测到语音会触发打断。
 */
#if CONFIG_AI_DIALOG_ENABLE_PLAYBACK_VAD
static void on_vad_state_change(bool speaking, void *context)
{
    (void)context;

    /* 播放开始后的短暂窗口只包含切换残留和扬声器回声，禁止触发打断。 */
    if (!playing() || now_ms() - atomic_load(&s_playback_since) < 500) {
        return;
    }

    /* 原子交换保证同一轮播放只产生一次打断请求。 */
    if (speaking && !atomic_exchange(&s_interrupt_requested, true)) {
        ESP_LOGI(TAG, "turn=%u VAD detected speech during playback, requesting interrupt",
                 atomic_load(&s_turn));
    }
}
#endif

static void acknowledge(unsigned turn, unsigned seq)
{
    coze_app_playback_progress(turn, seq);
}

/**
 * @brief 清理应用层音频资源并让逻辑层回到待机。
 *
 * 先将逻辑层置为待机，再等待编码、解码和播放任务退出当前数据，保证
 * 会话结束时不会再有旧会话写入统一音频服务。
 */
static void finish(const char *reason, bool notify, bool restore)
{
    bool active = ai_dialog_logic_is_session_active();
    ai_dialog_logic_reset();
    atomic_store(&s_wake_requested, false);
    atomic_store(&s_interrupt_requested, false);
    atomic_store(&s_interrupt_in_progress, false);
    if (active && notify) (void)coze_app_cancel(atomic_load(&s_turn));
    reset_capture();
    xSemaphoreTake(s_encoder_mutex, portMAX_DELAY);
    xSemaphoreTake(s_pcm_mutex, portMAX_DELAY);
    xSemaphoreTake(s_output_mutex, portMAX_DELAY);
    coze_app_end_turn(atomic_load(&s_turn));
    opus_ring_clear();
    clear_pcm();
    if (s_decoder != NULL) {
        (void)esp_opus_dec_reset(s_decoder);
    }
    (void)audio_service_set_output_enabled(
        AUDIO_SERVICE_OWNER_AI_DIALOG,
        false
    );
    /* 禁用 VAD 打断检测 */
    (void)audio_service_enable_vad(AUDIO_SERVICE_OWNER_AI_DIALOG, false);
    xSemaphoreGive(s_output_mutex); xSemaphoreGive(s_pcm_mutex); xSemaphoreGive(s_encoder_mutex);
    atomic_store(&s_failure, 0);
    wake_word_stop_conversation(); atomic_store(&s_end_seen, false);
    atomic_store(&s_restore_pending, restore);
    ESP_LOGI(TAG, "turn=%u session ended: %s", atomic_load(&s_turn), reason);
}
/*
 * 按 Coze 应用组件提供的音频协议打开本地上行编码器。
 * 音频格式由 coze_app 统一声明，AI 对话层只负责执行，不自行复制一套
 * 采样率、声道数或位深默认值。
 */
static bool open_codecs(const coze_app_audio_config_t *audio_config)
{
    if (audio_config == NULL || audio_config->input_sample_rate == 0 ||
        audio_config->output_sample_rate == 0 ||
        audio_config->input_channels == 0 || audio_config->output_channels == 0 ||
        audio_config->input_bits_per_sample == 0 || audio_config->output_bits_per_sample == 0) {
        return false;
    }
    /* 当前采集队列固定以 60 ms 组帧，Coze 配置必须与之保持一致。 */
    if (audio_config->input_frame_us != FRAME_MS * 1000U) {
        return false;
    }
    if (s_encoder) { esp_opus_enc_close(s_encoder); s_encoder = NULL; }
    if (s_decoder) { (void)esp_opus_dec_close(s_decoder); s_decoder = NULL; }
    s_input_opus = audio_config->input_opus;
    if (s_input_opus) {
        esp_opus_enc_config_t cfg = ESP_OPUS_ENC_CONFIG_DEFAULT();
        cfg.sample_rate = audio_config->input_sample_rate;
        cfg.channel = audio_config->input_channels;
        cfg.bits_per_sample = audio_config->input_bits_per_sample;
        cfg.bitrate = 24000; cfg.frame_duration = ESP_OPUS_ENC_FRAME_DURATION_60_MS;
        cfg.application_mode = ESP_OPUS_ENC_APPLICATION_VOIP; cfg.complexity = 5; cfg.enable_vbr = true;
        if (esp_opus_enc_open(&cfg, sizeof(cfg), &s_encoder) != ESP_AUDIO_ERR_OK) return false;
        int in = 0, out = 0;
        if (esp_opus_enc_get_frame_size(s_encoder, &in, &out) != ESP_AUDIO_ERR_OK ||
            in != FRAME_SAMPLES * sizeof(int16_t) || out > MAX_PACKET) {
            esp_opus_enc_close(s_encoder);
            s_encoder = NULL;
            return false;
        }
    }

    /* 下行线路使用 Opus，解码器固定输出 16 kHz、单声道 PCM 给扬声器。 */
    if (!audio_config->output_opus || audio_config->output_sample_rate != 16000U ||
        audio_config->output_channels != 1U || audio_config->output_bits_per_sample != 16U ||
        audio_config->output_frame_us != 20000U) {
        if (s_encoder != NULL) {
            esp_opus_enc_close(s_encoder);
            s_encoder = NULL;
        }
        return false;
    }
    esp_opus_dec_cfg_t decoder_config = ESP_OPUS_DEC_CONFIG_DEFAULT();
    decoder_config.sample_rate = audio_config->output_sample_rate;
    decoder_config.channel = audio_config->output_channels;
    decoder_config.frame_duration = ESP_OPUS_DEC_FRAME_DURATION_20_MS;
    decoder_config.self_delimited = false;
    if (esp_opus_dec_open(
            &decoder_config,
            sizeof(decoder_config),
            &s_decoder
        ) != ESP_AUDIO_ERR_OK) {
        s_decoder = NULL;
        if (s_encoder != NULL) {
            esp_opus_enc_close(s_encoder);
            s_encoder = NULL;
        }
        return false;
    }
    return true;
}
/**
 * @brief 播放唤醒提示音（水滴声）。
 *
 * 该函数在检测到唤醒后、开始录音前播放短促提示音，给用户明确的反馈。
 * 提示音时长约 300ms，播放期间暂停麦克风采集，避免回声干扰。
 */
static void play_wake_alert_sound(void)
{
    ESP_LOGI(TAG, "播放唤醒提示音");

    /* 临时启用扬声器 */
    esp_err_t error = audio_service_set_output_enabled(
        AUDIO_SERVICE_OWNER_AI_DIALOG,
        true
    );
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "启用扬声器失败: %s", esp_err_to_name(error));
        return;
    }

    /*
     * 原始提示音峰值为 16113，仅占 int16 PCM 满量程的约 49%。板级 DAC
     * 音量已经是 100%，这里将提示音归一化到接近 int16 满量程，避免继续
     * 受原始音频振幅限制。使用 32 位中间值，确保放大计算不会溢出。
     */
    const size_t chunk_size = 320; /* 20ms @ 16kHz */
    int16_t amplified_pcm[chunk_size];
    for (size_t offset = 0; offset < ALERT_DRIP_SAMPLE_COUNT; offset += chunk_size) {
        size_t samples_to_write = chunk_size;
        if (offset + samples_to_write > ALERT_DRIP_SAMPLE_COUNT) {
            samples_to_write = ALERT_DRIP_SAMPLE_COUNT - offset;
        }

        for (size_t index = 0; index < samples_to_write; ++index) {
            const int32_t amplified =
                ((int32_t)alert_drip_pcm[offset + index] * INT16_MAX) /
                ALERT_DRIP_PCM_PEAK;
            amplified_pcm[index] = (int16_t)amplified;
        }

        error = audio_service_write_pcm(
            AUDIO_SERVICE_OWNER_AI_DIALOG,
            amplified_pcm,
            samples_to_write
        );

        if (error != ESP_OK) {
            ESP_LOGW(TAG, "提示音写入失败: %s", esp_err_to_name(error));
            break;
        }
    }

    /* 等待播放完成 */
    error = audio_service_drain(AUDIO_SERVICE_OWNER_AI_DIALOG);
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "提示音 drain 失败: %s", esp_err_to_name(error));
    }

    /* 关闭扬声器 */
    error = audio_service_set_output_enabled(
        AUDIO_SERVICE_OWNER_AI_DIALOG,
        false
    );
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "关闭扬声器失败: %s", esp_err_to_name(error));
    }

    ESP_LOGI(TAG, "提示音播放完成");
}

static bool begin_turn(void)
{
    esp_err_t conversation_error = wake_word_start_conversation();
    if (conversation_error != ESP_OK) {
        ESP_LOGE(TAG, "开始对话轮次失败，音频所有权未切换：%s",
                 esp_err_to_name(conversation_error));
        return false;
    }
    reset_capture(); atomic_fetch_add(&s_turn, 1);
    atomic_store(&s_completed_seq, 0); atomic_store(&s_end_seen, false);
    atomic_store(&s_listen_since, now_ms());
    if (coze_app_begin_turn(atomic_load(&s_turn)) != ESP_OK) {
        atomic_store(&s_failure, 1);
        return false;
    }
    ESP_LOGI(TAG, "turn=%u listening: mode=local_vad pre-roll=300ms tail=%dms no-speech=%dms max=%dms", atomic_load(&s_turn),
        CONFIG_AI_DIALOG_TAIL_SILENCE_MS, CONFIG_AI_DIALOG_NO_SPEECH_MS, CONFIG_AI_DIALOG_MAX_TURN_MS);
    return true;
}
static void process_text(packet_t *packet)
{
    cJSON *root = cJSON_ParseWithLength((char *)packet->data, packet->length);
    if (!root) return;
    const char *type = json_str(root, "type");
    if (ai_dialog_logic_is_session_active() && json_uint(root, "turnId") == atomic_load(&s_turn)) {
        if (!strcmp(type, "audio_start") &&
            ai_dialog_logic_get_state() == AI_DIALOG_LOGIC_STATE_WAITING_REPLY) {
            (void)ai_dialog_logic_handle_event(AI_DIALOG_LOGIC_EVENT_AUDIO_STARTED);

            /* 仅在配置开启时启用播放期间的本地 VAD打断。 */
#if CONFIG_AI_DIALOG_ENABLE_PLAYBACK_VAD
            audio_service_enable_vad(AUDIO_SERVICE_OWNER_AI_DIALOG, true);
#endif

            if (audio_service_set_output_enabled(
                    AUDIO_SERVICE_OWNER_AI_DIALOG,
                    true
                ) != ESP_OK) {
                finish("speaker/capture transition failed", true, true);
                goto audio_start_done;
            }

            atomic_store(&s_audio_since, now_ms());
            atomic_store(&s_playback_since, now_ms());
            ESP_LOGI(TAG, "turn=%u first_audio after commit=%lldms", atomic_load(&s_turn), now_ms() - atomic_load(&s_wait_since));
            audio_start_done:;
        } else if (!strcmp(type, "audio_end") && playing()) {
            s_end_sequence = json_uint(root, "lastSequence");
            (void)ai_dialog_logic_handle_event(AI_DIALOG_LOGIC_EVENT_AUDIO_ENDED);
            atomic_store(&s_end_seen, true);
        } else if (!strcmp(type, "audio_stop")) {
            (void)ai_dialog_logic_handle_event(AI_DIALOG_LOGIC_EVENT_BACKEND_STOPPED);
            finish("Coze response stopped", false, true);
        } else if (!strcmp(type, "error")) {
            (void)ai_dialog_logic_handle_event(AI_DIALOG_LOGIC_EVENT_BACKEND_ERROR);
            finish(json_str(root, "message"), false, true);
        }
        else if (!strcmp(type, "transcript")) ESP_LOGI(TAG, "turn=%u transcript received", atomic_load(&s_turn));
    } else if (!strcmp(type, "error") && !json_uint(root, "turnId") && ai_dialog_logic_is_session_active()) {
        (void)ai_dialog_logic_handle_event(AI_DIALOG_LOGIC_EVENT_BACKEND_ERROR);
        finish(json_str(root, "message"), false, true);
    }
    cJSON_Delete(root);
}

/**
 * @brief 把直连后端事件复制到应用层控制/音频队列。
 *
 * Coze 回调运行在 WebSocket 任务中，不能在回调里执行状态迁移、编解码或
 * 播放。这里仅复制数据并零等待入队，所有后续动作由应用控制任务串行处理。
 */
/**
 * @brief 处理Coze事件（已移除，由app_main统一处理）
 *
 * 注意：在新架构中，Coze事件由app_main.c中的app_on_coze_event统一处理，
 * 然后通过状态机分发到各个业务模块。这里保留函数定义以兼容旧代码。
 */
#if 0  // 禁用此函数，不再使用
static void on_coze_event(const coze_app_event_t *event, void *context)
{
    (void)context;
    if (event == NULL || !atomic_load(&s_initialized)) {
        return;
    }

    if (event->type == COZE_APP_EVENT_AUDIO_DATA) {
        if (event->data == NULL || event->length == 0 || event->length > MAX_PACKET) {
            atomic_fetch_add(&s_rx_drops, 1);
            return;
        }
        unsigned evicted_count = 0;
        if (!opus_ring_push(
                event->data,
                event->length,
                event->turn,
                event->sequence,
                &evicted_count
            )) {
            atomic_fetch_add(&s_rx_drops, 1);
        } else if (evicted_count != 0) {
            /* 200 KB 已满时只淘汰最旧完整帧，统计被淘汰的帧数量。 */
            atomic_fetch_add(&s_rx_drops, evicted_count);
        }
        return;
    }

    char message[192];
    const char *type = "error";
    if (event->type == COZE_APP_EVENT_AUDIO_START) {
        type = "audio_start";
        snprintf(message, sizeof(message),
                 "{\"type\":\"%s\",\"turnId\":%u}", type, event->turn);
    } else if (event->type == COZE_APP_EVENT_AUDIO_END) {
        type = "audio_end";
        snprintf(message, sizeof(message),
                 "{\"type\":\"%s\",\"turnId\":%u,\"lastSequence\":%u}",
                 type, event->turn, event->sequence);
    } else if (event->type == COZE_APP_EVENT_DISCONNECTED) {
        snprintf(message, sizeof(message),
                 "{\"type\":\"error\",\"turnId\":%u,\"message\":\"Coze connection disconnected\"}",
                 event->turn);
    } else {
        snprintf(message, sizeof(message),
                 "{\"type\":\"error\",\"turnId\":%u,\"message\":\"Coze application error\"}",
                 event->turn);
    }

    packet_t packet = {
        .length = strlen(message),
        .data = malloc(strlen(message)),
        .turn = event->turn,
        .seq = event->sequence,
    };
    if (packet.data == NULL) {
        atomic_store(&s_failure, 4);
        return;
    }
    memcpy(packet.data, message, packet.length);
    if (xQueueSendToFront(s_control_queue, &packet, 0) != pdTRUE) {
        free(packet.data);
        atomic_store(&s_failure, 4);
    }
}
#endif  // 结束 #if 0

static bool capture_current(capture_t *frame)
{
    return frame->generation == atomic_load(&s_generation) && atomic_load(&s_ready) &&
        ai_dialog_logic_can_capture();
}
/* 32KB encoder stack only; encoder mutex held. */
static void encode_send(capture_t *frame)
{
    if (!capture_current(frame)) return;
    void *data = frame->samples; size_t size = sizeof(frame->samples);
    if (s_input_opus) {
        esp_audio_enc_in_frame_t in = { .buffer = data, .len = size };
        esp_audio_enc_out_frame_t out = { .buffer = s_encoded_buffer, .len = sizeof(s_encoded_buffer) };
        if (!s_encoder || esp_opus_enc_process(s_encoder, &in, &out) != ESP_AUDIO_ERR_OK || !out.encoded_bytes || out.encoded_bytes > MAX_PACKET) {
            atomic_fetch_add(&s_encode_drops, 1); return;
        }
        data = s_encoded_buffer; size = out.encoded_bytes;
    }
    if (capture_current(frame)) {
        if (coze_app_send_audio(data, size) == ESP_OK) { atomic_fetch_add(&s_encoded, 1); atomic_fetch_add(&s_tx_bytes, size); }
        else atomic_fetch_add(&s_encode_drops, 1);
    }
    atomic_store(&s_encoder_stack_free, uxTaskGetStackHighWaterMark(NULL));
}
static void ai_dialog_encode_worker(void *arg)
{
    (void)arg;
    capture_t frame;
    unsigned generation = UINT32_MAX, ring = 0, count = 0;
    ai_turn_t turn = {0};
    for (;;) {
        if (xQueueReceive(s_encode_queue, &frame, portMAX_DELAY) != pdTRUE) continue;
        xSemaphoreTake(s_encoder_mutex, portMAX_DELAY);
        if (!capture_current(&frame)) { xSemaphoreGive(s_encoder_mutex); continue; }
        if (generation != frame.generation) {
            generation = frame.generation; turn = (ai_turn_t){0}; ring = count = 0;
            vad_reset_trigger(s_vad);
        }
        {
            // server_vad 要求从唤醒后的第一帧开始连续上传，不能经过本地 VAD 筛选或发送 audio_end。
            encode_send(&frame);
        }
        {
            unsigned speech = 0;
            for (unsigned i = 0; i < FRAME_SAMPLES; i += 320) if (vad_process(s_vad, frame.samples + i, 16000, 20) == VAD_SPEECH) speech++;
            bool already_started = turn.started;
            ai_turn_event_t result = ai_turn_feed(&turn, speech >= 2, FRAME_MS,
                CONFIG_AI_DIALOG_TAIL_SILENCE_MS, CONFIG_AI_DIALOG_NO_SPEECH_MS, CONFIG_AI_DIALOG_MAX_TURN_MS);
            if (!already_started) { s_pre_roll[ring] = frame; ring = (ring + 1) % PRE_ROLL_FRAMES; if (count < PRE_ROLL_FRAMES) count++; }
            if (result == AI_TURN_STARTED) {
                ESP_LOGI(TAG, "turn=%u speech_start at +%ums", atomic_load(&s_turn), turn.elapsed_ms);
                unsigned oldest = (ring + PRE_ROLL_FRAMES - count) % PRE_ROLL_FRAMES;
                for (unsigned i = 0; i < count; ++i) encode_send(&s_pre_roll[(oldest + i) % PRE_ROLL_FRAMES]);
            } else if (already_started) encode_send(&frame);
            if (result == AI_TURN_COMMIT && capture_current(&frame)) {
                /* Current encoded frame includes the trailing silence. Partial raw
                 * capture after this boundary belongs to no submitted turn. */
                atomic_store(&s_wait_since, now_ms());
                (void)ai_dialog_logic_handle_event(AI_DIALOG_LOGIC_EVENT_TURN_COMMITTED);
                if (coze_app_commit_audio(atomic_load(&s_turn)) != ESP_OK) atomic_store(&s_failure, 1);
                ESP_LOGI(TAG, "turn=%u speech_end/commit at +%ums tail=%ums; upload stopped", atomic_load(&s_turn), turn.elapsed_ms, turn.silence_ms);
            } else if (result == AI_TURN_EMPTY) atomic_store(&s_failure, 2);
        }
        xSemaphoreGive(s_encoder_mutex);
    }
}
/**
 * @brief 把一个完整 Opus 帧解码成 PCM。
 *
 * Coze 的每个 audio delta 都是一个独立 Opus 帧。解码器使用固定的 20 ms、
 * 16 kHz、单声道配置；输入缓冲区不会被覆盖，输出缓冲区由调用方提供。
 */
static bool decode_opus_frame(
    const uint8_t *encoded,
    size_t encoded_length,
    uint8_t *pcm,
    size_t pcm_capacity,
    size_t *pcm_length
)
{
    if (s_decoder == NULL || encoded == NULL || encoded_length == 0 ||
        encoded_length > UINT32_MAX || pcm == NULL || pcm_length == NULL) {
        return false;
    }

    esp_audio_dec_in_raw_t raw = {
        .buffer = (uint8_t *)encoded,
        .len = (uint32_t)encoded_length,
        .consumed = 0,
        .frame_recover = ESP_AUDIO_DEC_RECOVERY_NONE,
    };
    esp_audio_dec_out_frame_t output = {
        .buffer = pcm,
        .len = (uint32_t)pcm_capacity,
        .needed_size = 0,
        .decoded_size = 0,
    };
    esp_audio_dec_info_t info = {0};
    size_t total_decoded = 0;

    while (raw.len > 0) {
        if (total_decoded >= pcm_capacity) {
            return false;
        }
        output.buffer = pcm + total_decoded;
        output.len = (uint32_t)(pcm_capacity - total_decoded);
        output.needed_size = 0;
        output.decoded_size = 0;
        esp_audio_err_t error = esp_opus_dec_decode(s_decoder, &raw, &output, &info);
        if (error != ESP_AUDIO_ERR_OK || output.decoded_size > pcm_capacity - total_decoded ||
            raw.consumed == 0 || raw.consumed > raw.len) {
            return false;
        }
        total_decoded += output.decoded_size;
        raw.buffer += raw.consumed;
        raw.len -= raw.consumed;
        raw.consumed = 0;
    }

    *pcm_length = total_decoded;
    return total_decoded != 0;
}

/**
 * @brief 把解码后的 PCM 送入有界播放队列。
 *
 * 播放任务按 I2S 实际速度消费该队列；当队列短暂满时，解码任务等待，
 * 让 200 KB Opus 缓冲区承担网络突发，避免在 WebSocket 回调中阻塞。
 */
static void enqueue_pcm(pcm_t *pcm)
{
    while (current(pcm->generation, pcm->turn) && playing()) if (xQueueSend(s_pcm_queue, pcm, pdMS_TO_TICKS(20)) == pdTRUE) return;
    free(pcm->data);
}

/**
 * @brief 独立的 Opus 解码任务。
 *
 * 该任务从 PSRAM 环形缓冲区按完整帧取数据，使用 ESP Audio Codec 的 Opus
 * 解码器生成 PCM，再把 PCM 交给现有扬声器播放任务。解码不在 Coze 回调中
 * 执行，因此网络接收和音频播放彼此不会直接阻塞。
 */
static void ai_dialog_decode_worker(void *arg)
{
    (void)arg;
    _Alignas(16) uint8_t opus_frame[MAX_PACKET];
    _Alignas(16) uint8_t pcm_buffer[MAX_PCM_BYTES];
    for (;;) {
        xSemaphoreTake(s_pcm_mutex, portMAX_DELAY);
        bool processed = false;
        size_t opus_length = 0;
        unsigned turn = 0;
        unsigned sequence = 0;
        if (playing() && opus_ring_pop(
                opus_frame,
                sizeof(opus_frame),
                &opus_length,
                &turn,
                &sequence
            )) {
            processed = true;
            unsigned gen = atomic_load(&s_generation);
            if (current(gen, turn)) {
                size_t pcm_length = 0;
                if (!decode_opus_frame(
                        opus_frame,
                        opus_length,
                        pcm_buffer,
                        sizeof(pcm_buffer),
                        &pcm_length
                    ) || pcm_length % sizeof(int16_t) != 0) {
                    atomic_fetch_add(&s_decode_drops, 1);
                    ESP_LOGW(TAG, "turn=%u Opus 解码失败 seq=%u bytes=%u",
                             turn, sequence, (unsigned)opus_length);
                } else {
                    pcm_t pcm = {
                        .length = pcm_length,
                        .data = heap_caps_malloc(
                            pcm_length,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
                        ),
                        .generation = gen,
                        .turn = turn,
                        .seq = sequence,
                    };
                    if (pcm.data == NULL) {
                        atomic_fetch_add(&s_decode_drops, 1);
                        ESP_LOGW(TAG, "turn=%u PCM PSRAM 分配失败 seq=%u bytes=%u",
                                 turn, sequence, (unsigned)pcm_length);
                    } else {
                        memcpy(pcm.data, pcm_buffer, pcm_length);
                        atomic_fetch_add(&s_decoded, 1);
                        enqueue_pcm(&pcm);
                    }
                }
            }
            atomic_store(&s_pcm_stack_free, uxTaskGetStackHighWaterMark(NULL));
        }
        xSemaphoreGive(s_pcm_mutex);
        if (!processed) {
            /* 没有待解码帧时睡眠；收到新帧后由回调释放信号立即唤醒。 */
            (void)xSemaphoreTake(s_opus_data_sem, pdMS_TO_TICKS(20));
        }
    }
}
static void ai_dialog_output_worker(void *arg)
{
    (void)arg;
    unsigned first_turn = 0;
    for (;;) {
        xSemaphoreTake(s_output_mutex, portMAX_DELAY);
        bool processed = false;
        pcm_t pcm;
        /* A short startup buffer absorbs Wi-Fi/timer jitter. Subsequent packets
         * stream directly; even short replies still drain through this worker. */
        if (playing() && now_ms() - atomic_load(&s_audio_since) >= 120 &&
            xQueueReceive(s_pcm_queue, &pcm, 0) == pdTRUE) {
            processed = true;
            esp_err_t error = ESP_OK;
            for (size_t i = 0; i < pcm.length && current(pcm.generation, pcm.turn) && playing(); ) {
                size_t bytes = pcm.length - i; if (bytes > 640) bytes = 640;
                error = audio_service_write_pcm(
                    AUDIO_SERVICE_OWNER_AI_DIALOG,
                    (int16_t *)(pcm.data + i),
                    bytes / 2
                );
                if (error != ESP_OK) break;
                i += bytes;
            }
            if (current(pcm.generation, pcm.turn) && playing()) {
                if (error != ESP_OK) { atomic_fetch_add(&s_write_drops, 1); ESP_LOGW(TAG, "Speaker write dropped: %s", esp_err_to_name(error)); }
                else if (pcm.length) {
                    atomic_fetch_add(&s_written, 1);
                    if (first_turn != pcm.turn) { first_turn = pcm.turn; ESP_LOGI(TAG, "turn=%u first_write +%lldms after first_audio", pcm.turn, now_ms() - atomic_load(&s_audio_since)); }
                }
                atomic_store(&s_completed_seq, pcm.seq); acknowledge(pcm.turn, pcm.seq);
            }
            free(pcm.data);
            atomic_store(&s_output_stack_free, uxTaskGetStackHighWaterMark(NULL));
        }
        xSemaphoreGive(s_output_mutex);
        if (!processed) vTaskDelay(1);
    }
}
/*
 * 音频结束事件不等于已经播放完成：必须同时确认 Opus 环形缓冲区、PCM 队列、
 * 解码任务和 I2S DMA 尾部都已经排空，才能切回待机监听。
 */
static void try_drain(void)
{
    if (!playing() || !atomic_load(&s_end_seen)) return;
    if (xSemaphoreTake(s_pcm_mutex, 0) != pdTRUE) return;
    if (xSemaphoreTake(s_output_mutex, 0) != pdTRUE) { xSemaphoreGive(s_pcm_mutex); return; }
    if (xSemaphoreTake(s_opus_mutex, 0) != pdTRUE) {
        xSemaphoreGive(s_output_mutex);
        xSemaphoreGive(s_pcm_mutex);
        return;
    }
    bool drained = s_opus_desc_count == 0 && !uxQueueMessagesWaiting(s_pcm_queue);
    xSemaphoreGive(s_opus_mutex);
    esp_err_t error = ESP_OK;
    if (drained) {
        error = audio_service_drain(AUDIO_SERVICE_OWNER_AI_DIALOG);
        (void)audio_service_set_output_enabled(
            AUDIO_SERVICE_OWNER_AI_DIALOG,
            false
        );
        acknowledge(atomic_load(&s_turn), s_end_sequence);
        ESP_LOGI(TAG, "turn=%u playback_drained seq=%u/%u dma=%s", atomic_load(&s_turn), atomic_load(&s_completed_seq), s_end_sequence, esp_err_to_name(error));
        (void)ai_dialog_logic_handle_event(AI_DIALOG_LOGIC_EVENT_PLAYBACK_DRAINED);
        atomic_store(&s_end_seen, false);
    }
    xSemaphoreGive(s_output_mutex); xSemaphoreGive(s_pcm_mutex);
    if (drained) {
        if (error == ESP_OK && begin_turn()) atomic_store(&s_restore_pending, true);
        else atomic_store(&s_failure, 3);
    }
}

/**
 * @brief 中断当前 AI 回复，但不关闭 Coze WebSocket。
 *
 * 播放打断和异常结束是两条不同的生命周期：异常结束可以关闭会话，
 * 播放打断必须保留长连接，立即开始新一轮对话。
 */
static void interrupt_playback(void)
{
    if (!playing() || !ai_dialog_logic_is_session_active()) {
        return;
    }
    /* 取消动作只允许同一轮播放执行一次，避免多个音频任务重复发送 cancel。 */
    if (atomic_exchange(&s_interrupt_in_progress, true)) {
        return;
    }

    const unsigned old_turn = atomic_load(&s_turn);
    ESP_LOGI(TAG, "turn=%u 检测到播放期间用户说话，开始打断", old_turn);

    /* 先让采集回调停止继续积累播放期数据，再清理所有旧回复缓存。 */
    ai_dialog_logic_reset();
    reset_capture();
    xSemaphoreTake(s_encoder_mutex, portMAX_DELAY);
    xSemaphoreTake(s_pcm_mutex, portMAX_DELAY);
    xSemaphoreTake(s_output_mutex, portMAX_DELAY);
    opus_ring_clear();
    clear_pcm();
    (void)audio_service_set_output_enabled(
        AUDIO_SERVICE_OWNER_AI_DIALOG,
        false
    );
    /* 禁用 VAD 打断检测 */
    (void)audio_service_enable_vad(AUDIO_SERVICE_OWNER_AI_DIALOG, false);
    atomic_store(&s_end_seen, false);
    xSemaphoreGive(s_output_mutex);
    xSemaphoreGive(s_pcm_mutex);
    xSemaphoreGive(s_encoder_mutex);

    /* Coze SDK 的该接口发送 conversation.chat.cancel，只取消当前回答。 */
    esp_err_t cancel_error = coze_app_cancel(old_turn);
    if (cancel_error != ESP_OK) {
        ESP_LOGE(TAG, "turn=%u 播放打断取消后端回复失败：%s", old_turn, esp_err_to_name(cancel_error));
        atomic_store(&s_failure, 1);
        atomic_store(&s_interrupt_in_progress, false);
        return;
    }

    /* 恢复逻辑状态后立即开始新 turn，旧网络事件会被 turn/generation 丢弃。 */
    (void)ai_dialog_logic_handle_event(AI_DIALOG_LOGIC_EVENT_WAKE_DETECTED);
    (void)ai_dialog_logic_handle_event(AI_DIALOG_LOGIC_EVENT_BACKEND_READY);
    if (!begin_turn()) {
        atomic_store(&s_failure, 1);
        return;
    }
    ESP_LOGI(TAG, "turn=%u 播放已停止，已进入新的用户语音输入轮次", atomic_load(&s_turn));
    atomic_store(&s_interrupt_in_progress, false);
}
static void ai_dialog_worker(void *arg)
{
    (void)arg;
    int64_t progress_at = now_ms();
#if CONFIG_AI_DIALOG_VERBOSE_STATS
    int64_t rate_at = now_ms();
#endif
    unsigned last_progress = 0;
    for (;;) {
        xSemaphoreTake(s_control_mutex, portMAX_DELAY);
        packet_t p;
        while (xQueueReceive(s_control_queue, &p, 0) == pdTRUE) { process_text(&p); free(p.data); }
        if (atomic_exchange(&s_interrupt_requested, false)) {
            interrupt_playback();
        }
        if (atomic_exchange(&s_wake_requested, false) &&
            ai_dialog_logic_get_state() == AI_DIALOG_LOGIC_STATE_IDLE) {
            (void)ai_dialog_logic_handle_event(AI_DIALOG_LOGIC_EVENT_WAKE_DETECTED);
            /*
             * 提示音使用 AI_DIALOG 所有者访问统一音频服务，因此必须在
             * 启动 Coze 和播放提示音之前完成 WAKE_WORD -> AI_DIALOG 转移。
             */
            esp_err_t conversation_error = wake_word_start_conversation();
            if (conversation_error != ESP_OK) {
                ai_dialog_logic_reset();
                ESP_LOGE(TAG, "唤醒后切换 AI 对话音频所有权失败：%s",
                         esp_err_to_name(conversation_error));
            } else if (atomic_load(&s_ready) && coze_app_is_connected()) {
                /* 播放唤醒提示音 */
                play_wake_alert_sound();

                (void)ai_dialog_logic_handle_event(AI_DIALOG_LOGIC_EVENT_BACKEND_READY);
                if (ai_dialog_logic_get_state() == AI_DIALOG_LOGIC_STATE_LISTENING) {
                    (void)begin_turn();
                } else {
                    finish("Coze state transition failed", false, true);
                }
            } else {
                ai_dialog_logic_reset();
                wake_word_stop_conversation();
                ESP_LOGW(TAG, "唤醒已忽略：AI 后端尚未就绪");
            }
        }
        unsigned failure = atomic_exchange(&s_failure, 0);
        if (failure && ai_dialog_logic_is_session_active()) finish(failure == 2 ? "no speech" : "audio pipeline failure", true, true);
        if (ai_dialog_logic_is_session_active()) {
            if (!coze_app_is_connected()) {
                atomic_store(&s_ready, false);
                (void)ai_dialog_logic_handle_event(AI_DIALOG_LOGIC_EVENT_BACKEND_STOPPED);
                finish("connection lost", false, true);
            }
            int64_t now = now_ms();
            ai_dialog_logic_state_t state = ai_dialog_logic_get_state();
            // server_vad 的录音阶段可能包含较长静音，不能用设备本地的无语音超时结束会话。
            if (state == AI_DIALOG_LOGIC_STATE_LISTENING && now - atomic_load(&s_listen_since) > CONFIG_AI_DIALOG_NO_SPEECH_MS + 300) finish("no speech", true, true);
            if (state == AI_DIALOG_LOGIC_STATE_LISTENING && now - atomic_load(&s_listen_since) > CONFIG_AI_DIALOG_MAX_TURN_MS + 2000) finish("capture watchdog", true, true);
            if (state == AI_DIALOG_LOGIC_STATE_WAITING_REPLY) {
                int64_t waited = now - atomic_load(&s_wait_since);
                if (waited >= 5000 && !s_wait_warning) { ESP_LOGW(TAG, "turn=%u still waiting for first audio (%lldms)", atomic_load(&s_turn), waited); s_wait_warning = true; }
                if (waited >= CONFIG_AI_DIALOG_REPLY_TIMEOUT_MS) finish("first audio timeout", true, true);
            } else s_wait_warning = false;
            if (state == AI_DIALOG_LOGIC_STATE_PLAYING) {
                unsigned completed = atomic_load(&s_completed_seq);
                if (completed != last_progress) { last_progress = completed; progress_at = now; }
                if (now - progress_at > 15000 && now - atomic_load(&s_audio_since) > 15000) finish("playback stalled", true, true);
                else try_drain();
            } else { progress_at = now; last_progress = 0; }
        }
        if (atomic_load(&s_restore_pending)) {
            esp_err_t err = wake_word_get_state() == WAKE_WORD_STATE_LISTENING
                ? ESP_OK : wake_word_start();
            if (err == ESP_OK) atomic_store(&s_restore_pending, false);
            else if (err != ESP_ERR_INVALID_STATE) ESP_LOGW(TAG, "Capture restore retry: %s", esp_err_to_name(err));
        }
#if CONFIG_AI_DIALOG_VERBOSE_STATS
        int64_t now = now_ms();
        if (now - rate_at >= 5000) {
            unsigned samples = atomic_exchange(&s_produced_samples, 0), enc = atomic_exchange(&s_encoded, 0);
            unsigned bytes = atomic_exchange(&s_tx_bytes, 0), dec = atomic_exchange(&s_decoded, 0), written = atomic_exchange(&s_written, 0);
            double seconds = (double)(now - rate_at) / 1000.0;
            ESP_LOGI(TAG, "audio %lldms PCM=%u samples %.2fKiB/s encoded=%u %.2ffps/%u bytes %.2fKiB/s decoded=%u written=%u; drops capture=%u encode/TX=%u RX=%u decode=%u write=%u; queues enc=%u/16 opus_packets=%u opus_bytes=%u/%u pcm=%u/4; encoder stack free=%u",
                now - rate_at, samples, samples * 2 / seconds / 1024, enc, enc / seconds, bytes, bytes / seconds / 1024, dec, written,
                atomic_exchange(&s_capture_drops, 0), atomic_exchange(&s_encode_drops, 0), atomic_exchange(&s_rx_drops, 0), atomic_exchange(&s_decode_drops, 0), atomic_exchange(&s_write_drops, 0),
                (unsigned)uxQueueMessagesWaiting(s_encode_queue), (unsigned)opus_ring_get_packet_count(),
                (unsigned)opus_ring_get_used_bytes(), (unsigned)OPUS_RING_BUFFER_BYTES,
                (unsigned)uxQueueMessagesWaiting(s_pcm_queue), atomic_load(&s_encoder_stack_free));
            rate_at = now;
            ESP_LOGI(TAG, "stack free encoder=%u/32768 decoder=%u/24576 output=%u/6144 control=%u/8192 bytes",
                atomic_load(&s_encoder_stack_free), atomic_load(&s_pcm_stack_free), atomic_load(&s_output_stack_free),
                (unsigned)uxTaskGetStackHighWaterMark(NULL));
        }
#endif
        xSemaphoreGive(s_control_mutex); vTaskDelay(pdMS_TO_TICKS(20));
    }
}
esp_err_t ai_dialog_init(const ai_dialog_config_t *config)
{
    // 新架构：验证配置参数
    if (config == NULL || config->on_event == NULL) {
        ESP_LOGE(TAG, "ai_dialog_init: 配置参数不能为空");
        return ESP_ERR_INVALID_ARG;
    }

    if (atomic_load(&s_initialized)) return ESP_ERR_INVALID_STATE;

    // 保存事件回调配置
    s_event_config = *config;

    if (ai_dialog_logic_init() != ESP_OK) return ESP_FAIL;
    s_control_queue = xQueueCreate(CONTROL_QUEUE_LENGTH, sizeof(packet_t));
    s_encode_queue = xQueueCreateWithCaps(ENCODE_QUEUE_LENGTH, sizeof(capture_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_pcm_queue = xQueueCreate(PCM_QUEUE_LENGTH, sizeof(pcm_t));
    s_control_mutex = xSemaphoreCreateMutex(); s_capture_mutex = xSemaphoreCreateMutex();
    s_encoder_mutex = xSemaphoreCreateMutex(); s_pcm_mutex = xSemaphoreCreateMutex(); s_output_mutex = xSemaphoreCreateMutex();
    s_opus_mutex = xSemaphoreCreateMutex(); s_opus_data_sem = xSemaphoreCreateBinary();
    s_vad = vad_create((vad_mode_t)CONFIG_AI_DIALOG_VAD_MODE);
    TaskHandle_t tasks[4] = {0};
    if (!s_control_queue || !s_encode_queue || !s_pcm_queue || !s_control_mutex || !s_capture_mutex ||
        !s_encoder_mutex || !s_pcm_mutex || !s_output_mutex || !s_opus_mutex || !s_opus_data_sem ||
        !s_vad ||
        opus_ring_init() != ESP_OK) goto fail;

    // 新架构：coze_app已在app_main的on_service_starting中初始化，这里不再初始化
    // 只获取音频配置
    coze_app_audio_config_t audio_config;
    if (coze_app_get_audio_config(&audio_config) != ESP_OK) {
        ESP_LOGE(TAG, "无法获取Coze音频配置");
        goto fail;
    }

    /* 仅在配置开启时注册播放期间的 VAD打断回调。 */
#if CONFIG_AI_DIALOG_ENABLE_PLAYBACK_VAD
    if (audio_service_set_vad_callback(on_vad_state_change, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register VAD callback");
        goto fail;
    }
#endif

    xSemaphoreTake(s_encoder_mutex, portMAX_DELAY);
    xSemaphoreTake(s_pcm_mutex, portMAX_DELAY);
    bool codec_ok = open_codecs(&audio_config);
    xSemaphoreGive(s_pcm_mutex);
    xSemaphoreGive(s_encoder_mutex);
    if (!codec_ok) goto fail;
    atomic_store(&s_ready, true);

    if (xTaskCreate(ai_dialog_encode_worker, "voice_ai_enc", AI_DIALOG_ENCODER_STACK_BYTES, NULL, 4, &tasks[0]) != pdPASS ||
        xTaskCreate(ai_dialog_decode_worker, "ai_dialog_pcm", AI_DIALOG_WORKER_STACK_BYTES, NULL, 4, &tasks[1]) != pdPASS ||
        xTaskCreate(ai_dialog_output_worker, "voice_ai_out", 6144, NULL, 5, &tasks[2]) != pdPASS ||
        xTaskCreate(ai_dialog_worker, "voice_ai_ctl", 8192, NULL, 5, &tasks[3]) != pdPASS) goto fail;
    atomic_store(&s_initialized, true);
    ESP_LOGI(TAG, "AI dialog initialized: playback VAD interrupt=%s",
#if CONFIG_AI_DIALOG_ENABLE_PLAYBACK_VAD
             "enabled"
#else
             "disabled"
#endif
    );
    return ESP_OK;
fail:
    for (unsigned i = 0; i < 4; ++i) if (tasks[i]) vTaskDelete(tasks[i]);
    if (s_decoder) { (void)esp_opus_dec_close(s_decoder); s_decoder = NULL; }
    if (s_encoder) { esp_opus_enc_close(s_encoder); s_encoder = NULL; }
    if (s_vad) vad_destroy(s_vad);
    opus_ring_deinit();
    QueueHandle_t queues[] = {s_control_queue, s_pcm_queue};
    for (unsigned i = 0; i < 2; ++i) if (queues[i]) vQueueDelete(queues[i]);
    if (s_encode_queue) vQueueDeleteWithCaps(s_encode_queue);
    SemaphoreHandle_t locks[] = {s_control_mutex, s_capture_mutex, s_encoder_mutex, s_pcm_mutex, s_output_mutex, s_opus_mutex, s_opus_data_sem};
    for (unsigned i = 0; i < 7; ++i) if (locks[i]) vSemaphoreDelete(locks[i]);
    return ESP_ERR_NO_MEM;
}
void ai_dialog_on_wake_detected(void)
{
    if (atomic_load(&s_initialized)) atomic_store(&s_wake_requested, true);
}

esp_err_t ai_dialog_start_session(void)
{
    if (!atomic_load(&s_initialized)) {
        ESP_LOGE(TAG, "AI对话服务未初始化");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "开始AI对话会话");

    // 触发唤醒请求
    atomic_store(&s_wake_requested, true);

    // 向状态机上报AI就绪事件
    emit_ai_event(AI_DIALOG_EVENT_READY, NULL);

    return ESP_OK;
}

void ai_dialog_deinit(void)
{
    if (!atomic_load(&s_initialized)) {
        return;
    }

    ESP_LOGI(TAG, "反初始化AI对话服务");

    // 停止当前会话
    ai_dialog_stop_session("反初始化");

    // TODO: 停止所有任务、释放资源
    // 当前保持简化实现，避免影响现有逻辑

    atomic_store(&s_initialized, false);
    ESP_LOGI(TAG, "AI对话服务已反初始化");
}

void ai_dialog_on_pcm(const int16_t *samples, size_t count, void *context)
{
    (void)context;
    if (!samples || !count || !atomic_load(&s_initialized)) return;
    const bool capture_allowed = ai_dialog_logic_can_capture();
    if (!capture_allowed) return;
    atomic_fetch_add(&s_produced_samples, count);
    if (xSemaphoreTake(s_capture_mutex, 0) != pdTRUE) { atomic_fetch_add(&s_capture_drops, 1); return; }

    /* 播放期间的打断检测已由 audio_service 的统一 VAD 处理，这里只处理正常录音 */
    if (ai_dialog_logic_can_capture()) {
        while (count) {
            size_t n = FRAME_SAMPLES - s_captured; if (n > count) n = count;
            memcpy(s_capture_frame.samples + s_captured, samples, n * sizeof(int16_t)); samples += n; count -= n; s_captured += n;
            if (s_captured != FRAME_SAMPLES) continue;
            s_capture_frame.generation = atomic_load(&s_generation);
            if (xQueueSend(s_encode_queue, &s_capture_frame, 0) != pdTRUE) {
                static capture_t discarded;
                if (xQueueReceive(s_encode_queue, &discarded, 0) == pdTRUE) atomic_fetch_add(&s_capture_drops, 1);
                if (xQueueSend(s_encode_queue, &s_capture_frame, 0) != pdTRUE) atomic_fetch_add(&s_capture_drops, 1);
            }
            s_captured = 0;
        }
    }
    xSemaphoreGive(s_capture_mutex);
}
esp_err_t ai_dialog_stop_session(const char *reason)
{
    if (!atomic_load(&s_initialized)) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_control_mutex, portMAX_DELAY);
    finish(reason ? reason : "external stop", true, false);
    xSemaphoreGive(s_control_mutex);
    return ESP_OK;
}
bool ai_dialog_is_session_active(void) { return ai_dialog_logic_is_session_active(); }
