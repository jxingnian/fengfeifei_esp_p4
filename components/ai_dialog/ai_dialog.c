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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_vad.h"
#include "encoder/impl/esp_opus_enc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "audio_service.h"
#include "wake_word.h"
#include "ai_turn.h"

#define AI_DIALOG_ENCODER_STACK_BYTES (32U * 1024U)
#define AI_DIALOG_WORKER_STACK_BYTES (24U * 1024U)
#define FRAME_SAMPLES 960U
#define FRAME_MS 60U
#define PRE_ROLL_FRAMES 5U
#define ENCODE_QUEUE_LENGTH 16U /* 960ms raw capture */
#define AUDIO_QUEUE_LENGTH 96U /* 为 Coze 下行突发保留约 1.9 秒 PCM 队列空间。 */
#define PCM_QUEUE_LENGTH 4U
#define MAX_PACKET 4096U
#define MAX_PCM_BYTES 8192U
#define CONTROL_QUEUE_LENGTH 32U

typedef struct { unsigned generation; int16_t samples[FRAME_SAMPLES]; } capture_t;
typedef struct { size_t length; uint8_t *data; unsigned turn, seq; } packet_t;
typedef struct { size_t length; uint8_t *data; unsigned generation, turn, seq; } pcm_t;
static const char *TAG = "ai_dialog";
static QueueHandle_t s_control_queue, s_audio_queue, s_encode_queue, s_pcm_queue;
static SemaphoreHandle_t s_control_mutex, s_encoder_mutex, s_pcm_mutex, s_output_mutex, s_capture_mutex;
static atomic_bool s_initialized, s_ready, s_wake_requested, s_restore_pending, s_speech_started;
static atomic_uint s_generation, s_turn, s_failure;
static atomic_uint s_produced_samples, s_encoded, s_decoded, s_written, s_tx_bytes;
static atomic_uint s_capture_drops, s_encode_drops, s_rx_drops, s_decode_drops, s_write_drops;
static atomic_uint s_completed_seq, s_encoder_stack_free, s_pcm_stack_free, s_output_stack_free;
static atomic_bool s_end_seen;
static atomic_llong s_listen_since, s_wait_since, s_audio_since;
static unsigned s_end_sequence;
static bool s_wait_warning, s_input_opus, s_server_vad;
static capture_t s_capture_frame;
static size_t s_captured;
static void *s_encoder;
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
    return ai_dialog_logic_get_state() == AI_DIALOG_LOGIC_STATE_PLAYING;
}
static bool current(unsigned generation, unsigned turn)
{
    return ai_dialog_logic_is_session_active() && generation == atomic_load(&s_generation) && turn == atomic_load(&s_turn);
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
    if (active && notify) (void)coze_app_cancel(atomic_load(&s_turn));
    reset_capture();
    xSemaphoreTake(s_encoder_mutex, portMAX_DELAY);
    xSemaphoreTake(s_pcm_mutex, portMAX_DELAY);
    xSemaphoreTake(s_output_mutex, portMAX_DELAY);
    coze_app_stop(); clear_packets(s_audio_queue); clear_pcm();
    (void)audio_service_set_output_enabled(
        AUDIO_SERVICE_OWNER_AI_DIALOG,
        false
    );
    xSemaphoreGive(s_output_mutex); xSemaphoreGive(s_pcm_mutex); xSemaphoreGive(s_encoder_mutex);
    atomic_store(&s_failure, 0);
    wake_word_stop_conversation(); atomic_store(&s_end_seen, false);
    atomic_store(&s_restore_pending, restore);
    coze_app_stop();
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
        if (esp_opus_enc_get_frame_size(s_encoder, &in, &out) != ESP_AUDIO_ERR_OK || in != FRAME_SAMPLES * sizeof(int16_t) || out > MAX_PACKET) return false;
    }
    /* Coze audio_callback 已经完成下行 Opus 解码，播放队列直接接收 PCM。 */
    if (audio_config->output_opus) return false;
    return true;
}
static bool begin_turn(void)
{
    wake_word_start_conversation();
    reset_capture(); atomic_fetch_add(&s_turn, 1);
    atomic_store(&s_completed_seq, 0); atomic_store(&s_end_seen, false); atomic_store(&s_speech_started, false);
    atomic_store(&s_listen_since, now_ms());
    if (coze_app_begin_turn(atomic_load(&s_turn)) != ESP_OK) {
        atomic_store(&s_failure, 1);
        return false;
    }
    ESP_LOGI(TAG, "turn=%u listening: mode=%s pre-roll=300ms tail=%dms no-speech=%dms max=%dms", atomic_load(&s_turn),
        s_server_vad ? "server_vad" : "client_interrupt",
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
            (ai_dialog_logic_get_state() == AI_DIALOG_LOGIC_STATE_WAITING_REPLY ||
             (s_server_vad && ai_dialog_logic_get_state() == AI_DIALOG_LOGIC_STATE_LISTENING))) {
            (void)ai_dialog_logic_handle_event(AI_DIALOG_LOGIC_EVENT_AUDIO_STARTED);
            if (wake_word_stop() != ESP_OK ||
                audio_service_set_output_enabled(
                    AUDIO_SERVICE_OWNER_AI_DIALOG,
                    true
                ) != ESP_OK) {
                finish("speaker/capture transition failed", true, true);
            }
            else {
                atomic_store(&s_audio_since, now_ms());
                ESP_LOGI(TAG, "turn=%u first_audio after commit=%lldms", atomic_load(&s_turn), now_ms() - atomic_load(&s_wait_since));
            }
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
            if (!s_server_vad) vad_reset_trigger(s_vad);
        }
        if (s_server_vad) {
            // server_vad 要求从唤醒后的第一帧开始连续上传，不能经过本地 VAD 筛选或发送 audio_end。
            encode_send(&frame);
        } else {
            unsigned speech = 0;
            for (unsigned i = 0; i < FRAME_SAMPLES; i += 320) if (vad_process(s_vad, frame.samples + i, 16000, 20) == VAD_SPEECH) speech++;
            bool already_started = turn.started;
            ai_turn_event_t result = ai_turn_feed(&turn, speech >= 2, FRAME_MS,
                CONFIG_AI_DIALOG_TAIL_SILENCE_MS, CONFIG_AI_DIALOG_NO_SPEECH_MS, CONFIG_AI_DIALOG_MAX_TURN_MS);
            if (!already_started) { s_pre_roll[ring] = frame; ring = (ring + 1) % PRE_ROLL_FRAMES; if (count < PRE_ROLL_FRAMES) count++; }
            if (result == AI_TURN_STARTED) {
                atomic_store(&s_speech_started, true);
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
/* Cancellation-aware bounded PCM queue. A zero-length marker retires a corrupt
 * packet in order, so one decoder failure cannot deadlock playback credit. */
static void enqueue_pcm(pcm_t *pcm)
{
    while (current(pcm->generation, pcm->turn) && playing()) if (xQueueSend(s_pcm_queue, pcm, pdMS_TO_TICKS(20)) == pdTRUE) return;
    free(pcm->data);
}
static void ai_dialog_decode_worker(void *arg)
{
    (void)arg;
    for (;;) {
        xSemaphoreTake(s_pcm_mutex, portMAX_DELAY);
        bool processed = false;
        packet_t p;
        if (playing() && xQueueReceive(s_audio_queue, &p, 0) == pdTRUE) {
            processed = true;
            unsigned gen = atomic_load(&s_generation);
            pcm_t pcm = { .generation = gen, .turn = p.turn, .seq = p.seq };
            if (current(gen, p.turn)) {
                pcm.data = heap_caps_malloc(MAX_PCM_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                bool ok = pcm.data != NULL;
                /* Coze 的 audio_callback 已经给出解码后的 PCM，直接复制到
                 * 播放队列，禁止在这里再次运行 Opus 解码器。 */
                ok = p.length <= MAX_PCM_BYTES && p.length % 2 == 0;
                if (ok) { memcpy(pcm.data, p.data, p.length); pcm.length = p.length; }
                if (!ok) {
                    atomic_fetch_add(&s_decode_drops, 1); free(pcm.data); pcm.data = NULL; pcm.length = 0;
                    ESP_LOGW(TAG, "turn=%u PCM dropped seq=%u bytes=%u", p.turn, p.seq, (unsigned)p.length);
                } else atomic_fetch_add(&s_decoded, 1);
                enqueue_pcm(&pcm);
            }
            free(p.data);
            atomic_store(&s_pcm_stack_free, uxTaskGetStackHighWaterMark(NULL));
        }
        xSemaphoreGive(s_pcm_mutex);
        /* A sub-tick delay rounds to zero at 100Hz and can starve idle/encoder.
         * Sleep a real tick only when idle, preserving short Opus frame rates. */
        if (!processed) vTaskDelay(1);
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
/* Empty compressed queue is NOT playback completion: check decoder, PCM,
 * in-flight writer and final DMA tail while holding both pipeline locks. */
static void try_drain(void)
{
    if (!playing() || !atomic_load(&s_end_seen)) return;
    if (xSemaphoreTake(s_pcm_mutex, 0) != pdTRUE) return;
    if (xSemaphoreTake(s_output_mutex, 0) != pdTRUE) { xSemaphoreGive(s_pcm_mutex); return; }
    bool drained = !uxQueueMessagesWaiting(s_audio_queue) && !uxQueueMessagesWaiting(s_pcm_queue);
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
        if (atomic_exchange(&s_wake_requested, false) &&
            ai_dialog_logic_get_state() == AI_DIALOG_LOGIC_STATE_IDLE) {
            (void)ai_dialog_logic_handle_event(AI_DIALOG_LOGIC_EVENT_WAKE_DETECTED);
            if (atomic_load(&s_ready) && coze_app_start() == ESP_OK) {
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
            if (!coze_app_connection_is_alive()) {
                atomic_store(&s_ready, false);
                (void)ai_dialog_logic_handle_event(AI_DIALOG_LOGIC_EVENT_BACKEND_STOPPED);
                finish("connection lost", false, true);
            }
            int64_t now = now_ms();
            ai_dialog_logic_state_t state = ai_dialog_logic_get_state();
            // server_vad 的录音阶段可能包含较长静音，不能用设备本地的无语音超时结束会话。
            if (state == AI_DIALOG_LOGIC_STATE_LISTENING && !s_server_vad && !atomic_load(&s_speech_started) && now - atomic_load(&s_listen_since) > CONFIG_AI_DIALOG_NO_SPEECH_MS + 300) finish("no speech", true, true);
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
        int64_t now = now_ms();
#if CONFIG_AI_DIALOG_VERBOSE_STATS
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
                atomic_load(&s_encoder_stack_free), atomic_load(&s_pcm_stack_free), atomic_load(&s_output_stack_free),
                (unsigned)uxTaskGetStackHighWaterMark(NULL));
        }
#endif
        xSemaphoreGive(s_control_mutex); vTaskDelay(pdMS_TO_TICKS(20));
    }
}
esp_err_t ai_dialog_init(void)
{
    if (atomic_load(&s_initialized)) return ESP_ERR_INVALID_STATE;
    if (ai_dialog_logic_init() != ESP_OK) return ESP_FAIL;
    s_control_queue = xQueueCreate(CONTROL_QUEUE_LENGTH, sizeof(packet_t)); s_audio_queue = xQueueCreate(AUDIO_QUEUE_LENGTH, sizeof(packet_t));
    s_encode_queue = xQueueCreateWithCaps(ENCODE_QUEUE_LENGTH, sizeof(capture_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_pcm_queue = xQueueCreate(PCM_QUEUE_LENGTH, sizeof(pcm_t));
    s_control_mutex = xSemaphoreCreateMutex(); s_capture_mutex = xSemaphoreCreateMutex();
    s_encoder_mutex = xSemaphoreCreateMutex(); s_pcm_mutex = xSemaphoreCreateMutex(); s_output_mutex = xSemaphoreCreateMutex();
    s_vad = vad_create((vad_mode_t)CONFIG_AI_DIALOG_VAD_MODE);
    TaskHandle_t tasks[4] = {0};
    if (!s_control_queue || !s_audio_queue || !s_encode_queue || !s_pcm_queue || !s_control_mutex || !s_capture_mutex || !s_encoder_mutex || !s_pcm_mutex || !s_output_mutex || !s_vad) goto fail;
    coze_app_config_t coze_config = {
        .on_event = on_coze_event,
        .context = NULL,
    };
    if (coze_app_init(&coze_config) != ESP_OK) goto fail;

    /* Coze 直连没有后台 ready 消息，启动时直接应用 Coze 组件声明的格式。 */
    coze_app_audio_config_t audio_config;
    if (coze_app_get_audio_config(&audio_config) != ESP_OK) goto fail;
    xSemaphoreTake(s_encoder_mutex, portMAX_DELAY);
    xSemaphoreTake(s_pcm_mutex, portMAX_DELAY);
    bool codec_ok = open_codecs(&audio_config);
    xSemaphoreGive(s_pcm_mutex);
    xSemaphoreGive(s_encoder_mutex);
    if (!codec_ok) goto fail;
    s_server_vad = audio_config.server_vad;
    atomic_store(&s_ready, true);

    if (xTaskCreate(ai_dialog_encode_worker, "voice_ai_enc", AI_DIALOG_ENCODER_STACK_BYTES, NULL, 4, &tasks[0]) != pdPASS ||
        xTaskCreate(ai_dialog_decode_worker, "ai_dialog_pcm", AI_DIALOG_WORKER_STACK_BYTES, NULL, 4, &tasks[1]) != pdPASS ||
        xTaskCreate(ai_dialog_output_worker, "voice_ai_out", 6144, NULL, 5, &tasks[2]) != pdPASS ||
        xTaskCreate(ai_dialog_worker, "voice_ai_ctl", 8192, NULL, 5, &tasks[3]) != pdPASS) goto fail;
    atomic_store(&s_initialized, true); return ESP_OK;
fail:
    for (unsigned i = 0; i < 4; ++i) if (tasks[i]) vTaskDelete(tasks[i]);
    if (s_vad) vad_destroy(s_vad);
    QueueHandle_t queues[] = {s_control_queue, s_audio_queue, s_pcm_queue};
    for (unsigned i = 0; i < 3; ++i) if (queues[i]) vQueueDelete(queues[i]);
    if (s_encode_queue) vQueueDeleteWithCaps(s_encode_queue);
    SemaphoreHandle_t locks[] = {s_control_mutex, s_capture_mutex, s_encoder_mutex, s_pcm_mutex, s_output_mutex};
    for (unsigned i = 0; i < 5; ++i) if (locks[i]) vSemaphoreDelete(locks[i]);
    return ESP_ERR_NO_MEM;
}
void ai_dialog_on_wake_detected(void)
{
    if (atomic_load(&s_initialized)) atomic_store(&s_wake_requested, true);
}
void ai_dialog_on_pcm(const int16_t *samples, size_t count, void *context)
{
    (void)context;
    if (!samples || !count || !atomic_load(&s_initialized) || !ai_dialog_logic_can_capture()) return;
    atomic_fetch_add(&s_produced_samples, count);
    if (xSemaphoreTake(s_capture_mutex, 0) != pdTRUE) { atomic_fetch_add(&s_capture_drops, 1); return; }
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
void ai_dialog_stop_session(const char *reason)
{
    if (!atomic_load(&s_initialized)) return;
    xSemaphoreTake(s_control_mutex, portMAX_DELAY);
    finish(reason ? reason : "external stop", true, false);
    xSemaphoreGive(s_control_mutex);
}
bool ai_dialog_is_session_active(void) { return ai_dialog_logic_is_session_active(); }
