/**
 * @file voice_ai_service.c
 * @brief AI 对话应用层。
 *
 * 本文件只负责把唤醒服务、音频流水线和抽象 AI 后端连接起来。会话状态由
 * voice_ai_logic 维护，Coze 与云端协议由 voice_ai_backend 维护；本文件不
 * 直接包含任何生成组件的内部实现。
 */
#include "voice_ai_service.h"
#include "voice_ai_backend.h"
#include "voice_ai_logic.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_vad.h"
#include "encoder/impl/esp_opus_enc.h"
#include "decoder/impl/esp_opus_dec.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "voice_board_audio.h"
#include "voice_service.h"
#include "voice_turn.h"

#define VOICE_AI_ENCODER_STACK_BYTES (32U * 1024U)
#define VOICE_AI_WORKER_STACK_BYTES (24U * 1024U)
#define FRAME_SAMPLES 960U
#define FRAME_MS 60U
#define PRE_ROLL_FRAMES 5U
#define ENCODE_QUEUE_LENGTH 16U /* 960ms raw capture */
#define AUDIO_QUEUE_LENGTH 96U /* backend credit <=24 packets/600ms */
#define PCM_QUEUE_LENGTH 4U
#define MAX_PACKET 4096U
#define MAX_PCM_BYTES 8192U
#define CONTROL_QUEUE_LENGTH 32U

typedef struct { unsigned generation; int16_t samples[FRAME_SAMPLES]; } capture_t;
typedef struct { size_t length; uint8_t *data; unsigned turn, seq; } packet_t;
typedef struct { size_t length; uint8_t *data; unsigned generation, turn, seq; } pcm_t;
static const char *TAG = "voice_ai_service";
static QueueHandle_t s_control_queue, s_audio_queue, s_encode_queue, s_pcm_queue;
static SemaphoreHandle_t s_control_mutex, s_encoder_mutex, s_decoder_mutex, s_output_mutex, s_capture_mutex;
static atomic_bool s_initialized, s_ready, s_wake_requested, s_restore_pending, s_speech_started;
static atomic_uint s_generation, s_turn, s_failure;
static atomic_uint s_produced_samples, s_encoded, s_decoded, s_written, s_tx_bytes;
static atomic_uint s_capture_drops, s_encode_drops, s_rx_drops, s_decode_drops, s_write_drops;
static atomic_uint s_completed_seq, s_encoder_stack_free, s_decoder_stack_free, s_output_stack_free;
static atomic_bool s_end_seen;
static atomic_llong s_listen_since, s_wait_since, s_audio_since;
static unsigned s_end_sequence;
static bool s_wait_warning, s_input_opus, s_output_opus, s_server_vad;
static capture_t s_capture_frame;
static size_t s_captured;
static void *s_encoder, *s_decoder;
static vad_handle_t s_vad;
static _Alignas(16) uint8_t s_encoded_buffer[MAX_PACKET];
static capture_t s_pre_roll[PRE_ROLL_FRAMES]; /* encoder owns ring */
static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }
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
    return voice_ai_logic_get_state() == VOICE_AI_LOGIC_STATE_PLAYING;
}
static bool current(unsigned generation, unsigned turn)
{
    return voice_ai_logic_is_session_active() && generation == atomic_load(&s_generation) && turn == atomic_load(&s_turn);
}
static void clear_packets(QueueHandle_t queue)
{
    packet_t p;
    while (xQueueReceive(queue, &p, 0) == pdTRUE) free(p.data);
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
static void acknowledge(unsigned turn, unsigned seq)
{
    voice_ai_backend_playback_progress(turn, seq);
}

/**
 * @brief 清理应用层音频资源并让逻辑层回到待机。
 *
 * 先将逻辑层置为待机，再等待编码、解码和播放任务退出当前数据，保证
 * 通话接管硬件时不会再有旧会话写入麦克风或扬声器。
 */
static void finish(const char *reason, bool notify, bool restore)
{
    bool active = voice_ai_logic_is_session_active();
    voice_ai_logic_reset();
    atomic_store(&s_wake_requested, false);
    if (active && notify) (void)voice_ai_backend_cancel(atomic_load(&s_turn));
    reset_capture();
    xSemaphoreTake(s_encoder_mutex, portMAX_DELAY);
    xSemaphoreTake(s_decoder_mutex, portMAX_DELAY);
    xSemaphoreTake(s_output_mutex, portMAX_DELAY);
    voice_ai_backend_stop(); clear_packets(s_audio_queue); clear_pcm();
    (void)voice_board_audio_set_output_enabled(false);
    xSemaphoreGive(s_output_mutex); xSemaphoreGive(s_decoder_mutex); xSemaphoreGive(s_encoder_mutex);
    atomic_store(&s_failure, 0);
    voice_service_stop_conversation(); atomic_store(&s_end_seen, false);
    atomic_store(&s_restore_pending, restore);
    voice_ai_backend_stop();
    ESP_LOGI(TAG, "turn=%u session ended: %s", atomic_load(&s_turn), reason);
}
static esp_opus_dec_frame_duration_t duration(unsigned us)
{
    switch (us) {
    case 2500: return ESP_OPUS_DEC_FRAME_DURATION_2_5_MS;
    case 5000: return ESP_OPUS_DEC_FRAME_DURATION_5_MS;
    case 10000: return ESP_OPUS_DEC_FRAME_DURATION_10_MS;
    case 20000: return ESP_OPUS_DEC_FRAME_DURATION_20_MS;
    case 40000: return ESP_OPUS_DEC_FRAME_DURATION_40_MS;
    case 60000: return ESP_OPUS_DEC_FRAME_DURATION_60_MS;
    case 80000: return ESP_OPUS_DEC_FRAME_DURATION_80_MS;
    case 100000: return ESP_OPUS_DEC_FRAME_DURATION_100_MS;
    case 120000: return ESP_OPUS_DEC_FRAME_DURATION_120_MS;
    default: return ESP_OPUS_DEC_FRAME_DURATION_INVALID;
    }
}
static bool open_codecs(bool input_opus, bool output_opus, unsigned frame_us)
{
    if (s_encoder) { esp_opus_enc_close(s_encoder); s_encoder = NULL; }
    if (s_decoder) { esp_opus_dec_close(s_decoder); s_decoder = NULL; }
    s_input_opus = input_opus; s_output_opus = output_opus;
    if (input_opus) {
        esp_opus_enc_config_t cfg = ESP_OPUS_ENC_CONFIG_DEFAULT();
        cfg.sample_rate = 16000; cfg.channel = 1; cfg.bits_per_sample = 16;
        cfg.bitrate = 24000; cfg.frame_duration = ESP_OPUS_ENC_FRAME_DURATION_60_MS;
        cfg.application_mode = ESP_OPUS_ENC_APPLICATION_VOIP; cfg.complexity = 5; cfg.enable_vbr = true;
        if (esp_opus_enc_open(&cfg, sizeof(cfg), &s_encoder) != ESP_AUDIO_ERR_OK) return false;
        int in = 0, out = 0;
        if (esp_opus_enc_get_frame_size(s_encoder, &in, &out) != ESP_AUDIO_ERR_OK || in != FRAME_SAMPLES * sizeof(int16_t) || out > MAX_PACKET) return false;
    }
    if (output_opus) {
        esp_opus_dec_cfg_t cfg = ESP_OPUS_DEC_CONFIG_DEFAULT();
        cfg.sample_rate = 16000; cfg.channel = 1; cfg.frame_duration = duration(frame_us); cfg.self_delimited = false;
        if (cfg.frame_duration == ESP_OPUS_DEC_FRAME_DURATION_INVALID || esp_opus_dec_open(&cfg, sizeof(cfg), &s_decoder) != ESP_AUDIO_ERR_OK) return false;
    }
    return true;
}
static bool audio_format(cJSON *format)
{
    return format && (!strcmp(json_str(format, "codec"), "opus") || !strcmp(json_str(format, "codec"), "pcm")) &&
        json_uint(format, "sampleRate") == 16000 && json_uint(format, "channel") == 1 && json_uint(format, "bitDepth") == 16;
}
static void ready(cJSON *root)
{
    atomic_store(&s_ready, false);
    if (voice_ai_logic_is_session_active()) finish("new audio configuration", false, true);
    cJSON *in = cJSON_GetObjectItemCaseSensitive(root, "input"), *out = cJSON_GetObjectItemCaseSensitive(root, "output");
    if (json_uint(root, "voiceProtocol") != 2 ||
        (strcmp(json_str(root, "turnDetection"), "client_interrupt") && strcmp(json_str(root, "turnDetection"), "server_vad")) ||
        strcmp(json_str(root, "provider"), "coze_realtime") || !audio_format(in) || !audio_format(out)) {
        ESP_LOGE(TAG, "Require backend voiceProtocol=2/client_interrupt-or-server_vad, PCM/Opus 16k mono16; upgrade backend first"); return;
    }
    // server_vad 下设备持续上传音频，由 Coze 服务端决定语音结束；client_interrupt 仍使用设备本地 VAD。
    s_server_vad = !strcmp(json_str(root, "turnDetection"), "server_vad");
    cJSON *frame = cJSON_GetObjectItemCaseSensitive(out, "frameSizeMs");
    unsigned us = cJSON_IsNumber(frame) ? (unsigned)(frame->valuedouble * 1000) : 20000;
    xSemaphoreTake(s_encoder_mutex, portMAX_DELAY); xSemaphoreTake(s_decoder_mutex, portMAX_DELAY);
    bool ok = open_codecs(!strcmp(json_str(in, "codec"), "opus"), !strcmp(json_str(out, "codec"), "opus"), us);
    xSemaphoreGive(s_decoder_mutex); xSemaphoreGive(s_encoder_mutex);
    if (ok) {
        atomic_store(&s_ready, true);
        ESP_LOGI(TAG, "云端音频配置就绪：input=%s output=%s, turn_detection=%s, output=%uus",
            json_str(in, "codec"), json_str(out, "codec"), s_server_vad ? "server_vad" : "client_interrupt", us);
    } else {
        ESP_LOGE(TAG, "云端音频编解码器初始化失败");
    }
}
static bool begin_turn(void)
{
    voice_service_start_conversation();
    reset_capture(); atomic_fetch_add(&s_turn, 1);
    atomic_store(&s_completed_seq, 0); atomic_store(&s_end_seen, false); atomic_store(&s_speech_started, false);
    atomic_store(&s_listen_since, now_ms());
    if (voice_ai_backend_begin_turn(atomic_load(&s_turn)) != ESP_OK) {
        atomic_store(&s_failure, 1);
        return false;
    }
    ESP_LOGI(TAG, "turn=%u listening: mode=%s pre-roll=300ms tail=%dms no-speech=%dms max=%dms", atomic_load(&s_turn),
        s_server_vad ? "server_vad" : "client_interrupt",
        CONFIG_VOICE_AI_TAIL_SILENCE_MS, CONFIG_VOICE_AI_NO_SPEECH_MS, CONFIG_VOICE_AI_MAX_TURN_MS);
    return true;
}
static void process_text(packet_t *packet)
{
    cJSON *root = cJSON_ParseWithLength((char *)packet->data, packet->length);
    if (!root) return;
    const char *type = json_str(root, "type");
    if (!strcmp(type, "ready")) ready(root);
    else if (voice_ai_logic_is_session_active() && json_uint(root, "turnId") == atomic_load(&s_turn)) {
        if (!strcmp(type, "audio_start") &&
            (voice_ai_logic_get_state() == VOICE_AI_LOGIC_STATE_WAITING_REPLY ||
             (s_server_vad && voice_ai_logic_get_state() == VOICE_AI_LOGIC_STATE_LISTENING))) {
            (void)voice_ai_logic_handle_event(VOICE_AI_LOGIC_EVENT_AUDIO_STARTED);
            if (voice_service_stop() != ESP_OK || voice_board_audio_set_output_enabled(true) != ESP_OK) finish("speaker/capture transition failed", true, true);
            else {
                atomic_store(&s_audio_since, now_ms());
                ESP_LOGI(TAG, "turn=%u first_audio after commit=%lldms", atomic_load(&s_turn), now_ms() - atomic_load(&s_wait_since));
            }
        } else if (!strcmp(type, "audio_end") && playing()) {
            s_end_sequence = json_uint(root, "lastSequence");
            (void)voice_ai_logic_handle_event(VOICE_AI_LOGIC_EVENT_AUDIO_ENDED);
            atomic_store(&s_end_seen, true);
        } else if (!strcmp(type, "audio_stop")) {
            (void)voice_ai_logic_handle_event(VOICE_AI_LOGIC_EVENT_BACKEND_STOPPED);
            finish("backend stopped response", false, true);
        } else if (!strcmp(type, "error")) {
            (void)voice_ai_logic_handle_event(VOICE_AI_LOGIC_EVENT_BACKEND_ERROR);
            finish(json_str(root, "message"), false, true);
        }
        else if (!strcmp(type, "transcript")) ESP_LOGI(TAG, "turn=%u transcript received", atomic_load(&s_turn));
    } else if (!strcmp(type, "error") && !json_uint(root, "turnId") && voice_ai_logic_is_session_active()) {
        (void)voice_ai_logic_handle_event(VOICE_AI_LOGIC_EVENT_BACKEND_ERROR);
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
static void on_backend_event(const voice_ai_backend_event_t *event, void *context)
{
    (void)context;
    if (event == NULL || !atomic_load(&s_initialized)) {
        return;
    }

    if (event->type == VOICE_AI_BACKEND_EVENT_AUDIO_DATA) {
        if (event->data == NULL || event->length == 0 || event->length > MAX_PACKET) {
            atomic_fetch_add(&s_rx_drops, 1);
            return;
        }

        packet_t packet = {
            .length = event->length,
            .data = malloc(event->length),
            .turn = event->turn,
            .seq = event->sequence,
        };
        if (packet.data == NULL) {
            atomic_fetch_add(&s_rx_drops, 1);
            return;
        }
        memcpy(packet.data, event->data, event->length);
        if (xQueueSend(s_audio_queue, &packet, 0) != pdTRUE) {
            packet_t discarded;
            if (xQueueReceive(s_audio_queue, &discarded, 0) == pdTRUE) {
                free(discarded.data);
                atomic_fetch_add(&s_rx_drops, 1);
            }
            if (xQueueSend(s_audio_queue, &packet, 0) != pdTRUE) {
                free(packet.data);
                atomic_fetch_add(&s_rx_drops, 1);
            }
        }
        return;
    }

    char message[192];
    const char *type = "error";
    if (event->type == VOICE_AI_BACKEND_EVENT_AUDIO_START) {
        type = "audio_start";
        snprintf(message, sizeof(message),
                 "{\"type\":\"%s\",\"turnId\":%u}", type, event->turn);
    } else if (event->type == VOICE_AI_BACKEND_EVENT_AUDIO_END) {
        type = "audio_end";
        snprintf(message, sizeof(message),
                 "{\"type\":\"%s\",\"turnId\":%u,\"lastSequence\":%u}",
                 type, event->turn, event->sequence);
    } else if (event->type == VOICE_AI_BACKEND_EVENT_DISCONNECTED) {
        snprintf(message, sizeof(message),
                 "{\"type\":\"error\",\"turnId\":%u,\"message\":\"AI backend disconnected\"}",
                 event->turn);
    } else {
        snprintf(message, sizeof(message),
                 "{\"type\":\"error\",\"turnId\":%u,\"message\":\"AI backend error\"}",
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
static bool capture_current(capture_t *frame)
{
    return frame->generation == atomic_load(&s_generation) && atomic_load(&s_ready) &&
        voice_ai_logic_can_capture();
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
        if (voice_ai_backend_send_audio(data, size) == ESP_OK) { atomic_fetch_add(&s_encoded, 1); atomic_fetch_add(&s_tx_bytes, size); }
        else atomic_fetch_add(&s_encode_drops, 1);
    }
    atomic_store(&s_encoder_stack_free, uxTaskGetStackHighWaterMark(NULL));
}
static void voice_ai_encode_worker(void *arg)
{
    (void)arg;
    capture_t frame;
    unsigned generation = UINT32_MAX, ring = 0, count = 0;
    voice_turn_t turn = {0};
    for (;;) {
        if (xQueueReceive(s_encode_queue, &frame, portMAX_DELAY) != pdTRUE) continue;
        xSemaphoreTake(s_encoder_mutex, portMAX_DELAY);
        if (!capture_current(&frame)) { xSemaphoreGive(s_encoder_mutex); continue; }
        if (generation != frame.generation) {
            generation = frame.generation; turn = (voice_turn_t){0}; ring = count = 0;
            if (!s_server_vad) vad_reset_trigger(s_vad);
        }
        if (s_server_vad) {
            // server_vad 要求从唤醒后的第一帧开始连续上传，不能经过本地 VAD 筛选或发送 audio_end。
            encode_send(&frame);
        } else {
            unsigned speech = 0;
            for (unsigned i = 0; i < FRAME_SAMPLES; i += 320) if (vad_process(s_vad, frame.samples + i, 16000, 20) == VAD_SPEECH) speech++;
            bool already_started = turn.started;
            voice_turn_event_t result = voice_turn_feed(&turn, speech >= 2, FRAME_MS,
                CONFIG_VOICE_AI_TAIL_SILENCE_MS, CONFIG_VOICE_AI_NO_SPEECH_MS, CONFIG_VOICE_AI_MAX_TURN_MS);
            if (!already_started) { s_pre_roll[ring] = frame; ring = (ring + 1) % PRE_ROLL_FRAMES; if (count < PRE_ROLL_FRAMES) count++; }
            if (result == TURN_STARTED) {
                atomic_store(&s_speech_started, true);
                ESP_LOGI(TAG, "turn=%u speech_start at +%ums", atomic_load(&s_turn), turn.elapsed_ms);
                unsigned oldest = (ring + PRE_ROLL_FRAMES - count) % PRE_ROLL_FRAMES;
                for (unsigned i = 0; i < count; ++i) encode_send(&s_pre_roll[(oldest + i) % PRE_ROLL_FRAMES]);
            } else if (already_started) encode_send(&frame);
            if (result == TURN_COMMIT && capture_current(&frame)) {
                /* Current encoded frame includes the trailing silence. Partial raw
                 * capture after this boundary belongs to no submitted turn. */
                atomic_store(&s_wait_since, now_ms());
                (void)voice_ai_logic_handle_event(VOICE_AI_LOGIC_EVENT_TURN_COMMITTED);
                if (voice_ai_backend_commit_audio(atomic_load(&s_turn)) != ESP_OK) atomic_store(&s_failure, 1);
                ESP_LOGI(TAG, "turn=%u speech_end/commit at +%ums tail=%ums; upload stopped", atomic_load(&s_turn), turn.elapsed_ms, turn.silence_ms);
            } else if (result == TURN_EMPTY) atomic_store(&s_failure, 2);
        }
        xSemaphoreGive(s_encoder_mutex);
    }
}
/* Cancellation-aware bounded PCM queue. A zero-length marker retires a corrupt
 * packet in order, so one decoder failure cannot deadlock playback credit. */
static void enqueue_pcm(pcm_t *pcm)
{
    while (current(pcm->generation, pcm->turn) && playing()) if (xQueueSend(s_pcm_queue, pcm, pdMS_TO_TICKS(20)) == pdTRUE) return;
    free(pcm->data);
}
static void voice_ai_decode_worker(void *arg)
{
    (void)arg;
    for (;;) {
        xSemaphoreTake(s_decoder_mutex, portMAX_DELAY);
        bool processed = false;
        packet_t p;
        if (playing() && xQueueReceive(s_audio_queue, &p, 0) == pdTRUE) {
            processed = true;
            unsigned gen = atomic_load(&s_generation);
            pcm_t pcm = { .generation = gen, .turn = p.turn, .seq = p.seq };
            if (current(gen, p.turn)) {
                pcm.data = heap_caps_malloc(MAX_PCM_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                bool ok = pcm.data != NULL;
                if (ok && s_output_opus) {
                    esp_audio_dec_in_raw_t raw = { .buffer = p.data, .len = p.length };
                    esp_audio_dec_out_frame_t out = { .buffer = pcm.data, .len = MAX_PCM_BYTES };
                    esp_audio_dec_info_t info = {0};
                    ok = s_decoder && esp_opus_dec_decode(s_decoder, &raw, &out, &info) == ESP_AUDIO_ERR_OK && raw.consumed == p.length &&
                        out.decoded_size > 0 && out.decoded_size <= MAX_PCM_BYTES && out.decoded_size % 2 == 0;
                    if (ok) pcm.length = out.decoded_size;
                } else if (ok) {
                    ok = p.length <= MAX_PCM_BYTES && p.length % 2 == 0;
                    if (ok) { memcpy(pcm.data, p.data, p.length); pcm.length = p.length; }
                }
                if (!ok) {
                    atomic_fetch_add(&s_decode_drops, 1); free(pcm.data); pcm.data = NULL; pcm.length = 0;
                    ESP_LOGW(TAG, "turn=%u decode dropped seq=%u bytes=%u", p.turn, p.seq, (unsigned)p.length);
                } else atomic_fetch_add(&s_decoded, 1);
                enqueue_pcm(&pcm);
            }
            free(p.data);
            atomic_store(&s_decoder_stack_free, uxTaskGetStackHighWaterMark(NULL));
        }
        xSemaphoreGive(s_decoder_mutex);
        /* A sub-tick delay rounds to zero at 100Hz and can starve idle/encoder.
         * Sleep a real tick only when idle, preserving short Opus frame rates. */
        if (!processed) vTaskDelay(1);
    }
}
static void voice_ai_output_worker(void *arg)
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
                error = voice_board_audio_write((int16_t *)(pcm.data + i), bytes / 2);
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
/* Empty compressed queue is NOT playback completion: check decoder, PCM,
 * in-flight writer and final DMA tail while holding both pipeline locks. */
static void try_drain(void)
{
    if (!playing() || !atomic_load(&s_end_seen)) return;
    if (xSemaphoreTake(s_decoder_mutex, 0) != pdTRUE) return;
    if (xSemaphoreTake(s_output_mutex, 0) != pdTRUE) { xSemaphoreGive(s_decoder_mutex); return; }
    bool drained = !uxQueueMessagesWaiting(s_audio_queue) && !uxQueueMessagesWaiting(s_pcm_queue);
    esp_err_t error = ESP_OK;
    if (drained) {
        error = voice_board_audio_drain(); (void)voice_board_audio_set_output_enabled(false);
        acknowledge(atomic_load(&s_turn), s_end_sequence);
        ESP_LOGI(TAG, "turn=%u playback_drained seq=%u/%u dma=%s", atomic_load(&s_turn), atomic_load(&s_completed_seq), s_end_sequence, esp_err_to_name(error));
        (void)voice_ai_logic_handle_event(VOICE_AI_LOGIC_EVENT_PLAYBACK_DRAINED);
        atomic_store(&s_end_seen, false);
    }
    xSemaphoreGive(s_output_mutex); xSemaphoreGive(s_decoder_mutex);
    if (drained) {
        if (error == ESP_OK && begin_turn()) atomic_store(&s_restore_pending, true);
        else atomic_store(&s_failure, 3);
    }
}
static void voice_ai_worker(void *arg)
{
    (void)arg;
    int64_t progress_at = now_ms();
#if CONFIG_VOICE_AI_VERBOSE_STATS
    int64_t rate_at = now_ms();
#endif
    unsigned last_progress = 0;
    for (;;) {
        xSemaphoreTake(s_control_mutex, portMAX_DELAY);
        packet_t p;
        while (xQueueReceive(s_control_queue, &p, 0) == pdTRUE) { process_text(&p); free(p.data); }
        if (atomic_exchange(&s_wake_requested, false) &&
            voice_ai_logic_get_state() == VOICE_AI_LOGIC_STATE_IDLE) {
            (void)voice_ai_logic_handle_event(VOICE_AI_LOGIC_EVENT_WAKE_DETECTED);
            if (atomic_load(&s_ready) && voice_ai_backend_start() == ESP_OK) {
                (void)voice_ai_logic_handle_event(VOICE_AI_LOGIC_EVENT_BACKEND_READY);
                if (voice_ai_logic_get_state() == VOICE_AI_LOGIC_STATE_LISTENING) {
                    (void)begin_turn();
                } else {
                    finish("backend state transition failed", false, true);
                }
            } else {
                voice_ai_logic_reset();
                voice_service_stop_conversation();
                ESP_LOGW(TAG, "唤醒已忽略：AI 后端尚未就绪");
            }
        }
        unsigned failure = atomic_exchange(&s_failure, 0);
        if (failure && voice_ai_logic_is_session_active()) finish(failure == 2 ? "no speech" : "audio pipeline failure", true, true);
        if (voice_ai_logic_is_session_active()) {
            if (!voice_ai_backend_connection_is_alive()) {
                atomic_store(&s_ready, false);
                (void)voice_ai_logic_handle_event(VOICE_AI_LOGIC_EVENT_BACKEND_STOPPED);
                finish("connection lost", false, true);
            }
            int64_t now = now_ms();
            voice_ai_logic_state_t state = voice_ai_logic_get_state();
            // server_vad 的录音阶段可能包含较长静音，不能用设备本地的无语音超时结束会话。
            if (state == VOICE_AI_LOGIC_STATE_LISTENING && !s_server_vad && !atomic_load(&s_speech_started) && now - atomic_load(&s_listen_since) > CONFIG_VOICE_AI_NO_SPEECH_MS + 300) finish("no speech", true, true);
            if (state == VOICE_AI_LOGIC_STATE_LISTENING && now - atomic_load(&s_listen_since) > CONFIG_VOICE_AI_MAX_TURN_MS + 2000) finish("capture watchdog", true, true);
            if (state == VOICE_AI_LOGIC_STATE_WAITING_REPLY) {
                int64_t waited = now - atomic_load(&s_wait_since);
                if (waited >= 5000 && !s_wait_warning) { ESP_LOGW(TAG, "turn=%u still waiting for first audio (%lldms)", atomic_load(&s_turn), waited); s_wait_warning = true; }
                if (waited >= CONFIG_VOICE_AI_REPLY_TIMEOUT_MS) finish("first audio timeout", true, true);
            } else s_wait_warning = false;
            if (state == VOICE_AI_LOGIC_STATE_PLAYING) {
                unsigned completed = atomic_load(&s_completed_seq);
                if (completed != last_progress) { last_progress = completed; progress_at = now; }
                if (now - progress_at > 15000 && now - atomic_load(&s_audio_since) > 15000) finish("playback stalled", true, true);
                else try_drain();
            } else { progress_at = now; last_progress = 0; }
        }
        if (atomic_load(&s_restore_pending)) {
            esp_err_t err = voice_service_get_state() == VOICE_SERVICE_STATE_LISTENING
                ? ESP_OK : voice_service_start();
            if (err == ESP_OK) atomic_store(&s_restore_pending, false);
            else if (err != ESP_ERR_INVALID_STATE) ESP_LOGW(TAG, "Capture restore retry: %s", esp_err_to_name(err));
        }
        int64_t now = now_ms();
#if CONFIG_VOICE_AI_VERBOSE_STATS
        if (now - rate_at >= 5000) {
            unsigned samples = atomic_exchange(&s_produced_samples, 0), enc = atomic_exchange(&s_encoded, 0);
            unsigned bytes = atomic_exchange(&s_tx_bytes, 0), dec = atomic_exchange(&s_decoded, 0), written = atomic_exchange(&s_written, 0);
            double seconds = (double)(now - rate_at) / 1000.0;
            ESP_LOGI(TAG, "audio %lldms PCM=%u samples %.2fKiB/s encoded=%u %.2ffps/%u bytes %.2fKiB/s decoded=%u written=%u; drops capture=%u encode/TX=%u RX=%u decode=%u write=%u; queues enc=%u/16 opus=%u/96 pcm=%u/4; encoder stack free=%u",
                now - rate_at, samples, samples * 2 / seconds / 1024, enc, enc / seconds, bytes, bytes / seconds / 1024, dec, written,
                atomic_exchange(&s_capture_drops, 0), atomic_exchange(&s_encode_drops, 0), atomic_exchange(&s_rx_drops, 0), atomic_exchange(&s_decode_drops, 0), atomic_exchange(&s_write_drops, 0),
                (unsigned)uxQueueMessagesWaiting(s_encode_queue), (unsigned)uxQueueMessagesWaiting(s_audio_queue), (unsigned)uxQueueMessagesWaiting(s_pcm_queue), atomic_load(&s_encoder_stack_free));
            rate_at = now;
            ESP_LOGI(TAG, "stack free encoder=%u/32768 decoder=%u/24576 output=%u/6144 control=%u/8192 bytes",
                atomic_load(&s_encoder_stack_free), atomic_load(&s_decoder_stack_free), atomic_load(&s_output_stack_free),
                (unsigned)uxTaskGetStackHighWaterMark(NULL));
        }
#endif
        xSemaphoreGive(s_control_mutex); vTaskDelay(pdMS_TO_TICKS(20));
    }
}
esp_err_t voice_ai_service_init(void)
{
    if (atomic_load(&s_initialized)) return ESP_ERR_INVALID_STATE;
    if (voice_ai_logic_init() != ESP_OK) return ESP_FAIL;
    s_control_queue = xQueueCreate(CONTROL_QUEUE_LENGTH, sizeof(packet_t)); s_audio_queue = xQueueCreate(AUDIO_QUEUE_LENGTH, sizeof(packet_t));
    s_encode_queue = xQueueCreateWithCaps(ENCODE_QUEUE_LENGTH, sizeof(capture_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_pcm_queue = xQueueCreate(PCM_QUEUE_LENGTH, sizeof(pcm_t));
    s_control_mutex = xSemaphoreCreateMutex(); s_capture_mutex = xSemaphoreCreateMutex();
    s_encoder_mutex = xSemaphoreCreateMutex(); s_decoder_mutex = xSemaphoreCreateMutex(); s_output_mutex = xSemaphoreCreateMutex();
    s_vad = vad_create((vad_mode_t)CONFIG_VOICE_AI_VAD_MODE);
    TaskHandle_t tasks[4] = {0};
    if (!s_control_queue || !s_audio_queue || !s_encode_queue || !s_pcm_queue || !s_control_mutex || !s_capture_mutex || !s_encoder_mutex || !s_decoder_mutex || !s_output_mutex || !s_vad) goto fail;
    voice_ai_backend_config_t backend_config = {
        .on_event = on_backend_event,
        .context = NULL,
    };
    if (voice_ai_backend_init(&backend_config) != ESP_OK) goto fail;

    /* 直连模式没有云端 ready 消息，因此应用层在启动时固定初始化 Coze 音频格式。 */
    if (voice_ai_backend_is_direct()) {
        voice_ai_backend_audio_config_t audio_config;
        if (voice_ai_backend_get_audio_config(&audio_config) != ESP_OK) goto fail;
        xSemaphoreTake(s_encoder_mutex, portMAX_DELAY);
        xSemaphoreTake(s_decoder_mutex, portMAX_DELAY);
        bool codec_ok = open_codecs(audio_config.input_opus,
                                    audio_config.output_opus,
                                    audio_config.output_frame_us);
        xSemaphoreGive(s_decoder_mutex);
        xSemaphoreGive(s_encoder_mutex);
        if (!codec_ok) goto fail;
        s_server_vad = audio_config.server_vad;
        atomic_store(&s_ready, true);
    }

    if (xTaskCreate(voice_ai_encode_worker, "voice_ai_enc", VOICE_AI_ENCODER_STACK_BYTES, NULL, 4, &tasks[0]) != pdPASS ||
        xTaskCreate(voice_ai_decode_worker, "voice_ai_dec", VOICE_AI_WORKER_STACK_BYTES, NULL, 4, &tasks[1]) != pdPASS ||
        xTaskCreate(voice_ai_output_worker, "voice_ai_out", 6144, NULL, 5, &tasks[2]) != pdPASS ||
        xTaskCreate(voice_ai_worker, "voice_ai_ctl", 8192, NULL, 5, &tasks[3]) != pdPASS) goto fail;
    atomic_store(&s_initialized, true); return ESP_OK;
fail:
    for (unsigned i = 0; i < 4; ++i) if (tasks[i]) vTaskDelete(tasks[i]);
    if (s_vad) vad_destroy(s_vad);
    QueueHandle_t queues[] = {s_control_queue, s_audio_queue, s_pcm_queue};
    for (unsigned i = 0; i < 3; ++i) if (queues[i]) vQueueDelete(queues[i]);
    if (s_encode_queue) vQueueDeleteWithCaps(s_encode_queue);
    SemaphoreHandle_t locks[] = {s_control_mutex, s_capture_mutex, s_encoder_mutex, s_decoder_mutex, s_output_mutex};
    for (unsigned i = 0; i < 5; ++i) if (locks[i]) vSemaphoreDelete(locks[i]);
    return ESP_ERR_NO_MEM;
}
void voice_ai_service_on_wake_detected(void)
{
    if (atomic_load(&s_initialized)) atomic_store(&s_wake_requested, true);
}
void voice_ai_service_on_pcm(const int16_t *samples, size_t count, void *context)
{
    (void)context;
    if (!samples || !count || !atomic_load(&s_initialized) || !voice_ai_logic_can_capture()) return;
    atomic_fetch_add(&s_produced_samples, count);
    if (xSemaphoreTake(s_capture_mutex, 0) != pdTRUE) { atomic_fetch_add(&s_capture_drops, 1); return; }
    if (voice_ai_logic_can_capture()) {
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
static unsigned read_le32(const uint8_t *p)
{
    return (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}
void voice_ai_service_on_cloud_message(bool binary, const void *data, size_t length)
{
    /* 直连模式只保留云端网关的设备管理和通话用途，禁止云端 AI 消息进入队列。 */
    if (voice_ai_backend_is_direct() || !atomic_load(&s_initialized) || !data || !length) return;
    packet_t p = {0}; bool urgent = false;
    if (binary) {
        const uint8_t *b = data;
        if (!voice_ai_logic_is_session_active() || length < 13 || length > MAX_PACKET + 12 || b[0] != 0x56 || b[1] != 0x41 || b[2] != 2 || b[3] != 0) return;
        p.turn = read_le32(b + 4); p.seq = read_le32(b + 8);
        if (!p.seq || p.turn != atomic_load(&s_turn)) return;
        data = b + 12; length -= 12;
    } else {
        if (length > 16384) { atomic_store(&s_failure, 4); return; }
        cJSON *root = cJSON_ParseWithLength(data, length); if (!root) return;
        const char *type = json_str(root, "type");
        bool accepted = !strcmp(type, "ready") || !strcmp(type, "audio_start") || !strcmp(type, "audio_end") || !strcmp(type, "audio_stop") || !strcmp(type, "error") || !strcmp(type, "transcript");
        urgent = !strcmp(type, "audio_stop") || !strcmp(type, "error"); cJSON_Delete(root); if (!accepted) return;
    }
    p.length = length; p.data = malloc(length);
    if (!p.data) { if (binary) atomic_fetch_add(&s_rx_drops, 1); else atomic_store(&s_failure, 4); return; }
    memcpy(p.data, data, length);
    QueueHandle_t queue = binary ? s_audio_queue : s_control_queue;
    BaseType_t sent = urgent ? xQueueSendToFront(queue, &p, 0) : xQueueSend(queue, &p, 0);
    if (sent != pdTRUE) {
        if (binary) {
            packet_t old;
            if (xQueueReceive(queue, &old, 0) == pdTRUE) { free(old.data); atomic_fetch_add(&s_rx_drops, 1); }
            if (xQueueSend(queue, &p, 0) == pdTRUE) return;
            atomic_fetch_add(&s_rx_drops, 1);
        } else atomic_store(&s_failure, 4);
        free(p.data);
    }
}
void voice_ai_service_stop_session(const char *reason)
{
    if (!atomic_load(&s_initialized)) return;
    xSemaphoreTake(s_control_mutex, portMAX_DELAY);
    finish(reason ? reason : "external stop", true, false);
    xSemaphoreGive(s_control_mutex);
}
bool voice_ai_service_is_session_active(void) { return voice_ai_logic_is_session_active(); }
