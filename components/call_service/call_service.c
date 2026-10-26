#include "call_service.h"
#include "call_media.h"
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <ctype.h>
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "cloud_gateway.h"
#include "voice_board_audio.h"
#include "voice_service.h"
#include "voice_ai_service.h"

typedef struct { size_t length; bool binary; uint8_t *data; int64_t received; } message_t;
static QueueHandle_t s_queue;
static call_service_send_t s_send;
static atomic_bool s_overflow;
static bool s_active, s_rtc, s_accepted, s_voice_stopped, s_camera_ready;
static bool s_voice_resume_pending;
static char s_call_id[80];
static int64_t s_deadline, s_reconnect_deadline;
static int64_t s_resume_deadline;
static const char *TAG = "call_service";

static const char *field(cJSON *root, const char *name)
{
    const char *value = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, name));
    return value ? value : "";
}

/**
 * @brief 判断后台文字消息是否属于通话服务。
 *
 * AI 语音事件和通话事件共用后台 WebSocket。通话服务只接收 ready、
 * call_offer、call_end 三类消息，其他消息在进入通话队列前直接丢弃，
 * 避免 Coze 的高频文字事件和 AI 音频占满通话队列。
 */
static bool is_call_text_message(const void *data, size_t length)
{
    cJSON *root = cJSON_ParseWithLength((const char *)data, length);
    if (root == NULL) {
        return false;
    }

    const char *type = field(root, "type");
    bool accepted = !strcmp(type, "ready") || !strcmp(type, "call_offer") ||
                    !strcmp(type, "call_end");
    cJSON_Delete(root);
    return accepted;
}

static void signal_call(const char *type, const char *id, const char *reason)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", type);
    cJSON_AddStringToObject(root, "callId", id);
    if (reason) cJSON_AddStringToObject(root, "reason", reason);
    char *text = cJSON_PrintUnformatted(root);
    if (text && s_send) (void)s_send(false, text, strlen(text));
    free(text); cJSON_Delete(root);
}

static void resume_voice(void)
{
    /* A timed-out stop still requests task exit. Do not reopen a codec while
     * the recognition task may still be reading it. Retry from the worker. */
    esp_err_t error = voice_service_stop();
    if (error == ESP_OK) error = voice_board_audio_restore();
    if (error == ESP_OK) error = voice_service_start();
    s_voice_resume_pending = error != ESP_OK;
    s_resume_deadline = esp_timer_get_time() + 2000000;
    if (error != ESP_OK) ESP_LOGW(TAG, "Voice resume deferred: %s", esp_err_to_name(error));
}

static void finish(const char *reason, bool notify)
{
    if (!s_active) return;
    if (notify) signal_call(s_accepted ? "call_end" : "call_reject", s_call_id, reason);
    s_active = false;
    if (s_rtc) call_media_close();
    (void)voice_board_audio_set_output_enabled(false);
    if (s_voice_stopped) resume_voice();
    s_voice_stopped = false; s_call_id[0] = 0; s_deadline = s_reconnect_deadline = 0;
    ESP_LOGI(TAG, "Call finished: %s", reason);
}

static void capabilities(void)
{
    const char *text = s_camera_ready
        ? "{\"type\":\"device_capabilities\",\"capabilities\":[\"microphone\",\"speaker\",\"ai-voice\",\"coze-realtime-pcm\",\"camera\",\"webrtc\",\"rtc-livekit\"]}"
        : "{\"type\":\"device_capabilities\",\"capabilities\":[\"microphone\",\"speaker\",\"ai-voice\",\"coze-realtime-pcm\"]}";
    (void)s_send(false, text, strlen(text));
}

static void process(message_t *item)
{
    if (item->binary) {
        if (s_active && !s_rtc && s_accepted && item->length % 2 == 0)
            (void)voice_board_audio_write((int16_t *)item->data, item->length / 2);
        return;
    }
    cJSON *root = cJSON_Parse((char *)item->data);
    if (!root) return;
    const char *type = field(root, "type"), *id = field(root, "callId");
    if (!strcmp(type, "ready")) { capabilities(); goto done; }
    if (!strcmp(type, "call_end")) {
        if (s_active && !strcmp(id, s_call_id)) finish("remote_hangup", false);
        goto done;
    }
    if (strcmp(type, "call_offer")) goto done;
    if (!id[0] || strlen(id) >= sizeof(s_call_id)) goto done;
    for (const char *p = id; *p; ++p) if (!isalnum((unsigned char)*p) && *p != '-' && *p != '_') goto done;
    if (s_active) {
        if (strcmp(id, s_call_id)) signal_call("call_reject", id, "busy");
        goto done;
    }
    if (s_voice_resume_pending) { signal_call("call_reject", id, "audio_recovering"); goto done; }
    const char *media = field(root, "media");
    bool rtc = !strcmp(media, "webrtc");
    if (media[0] && !rtc && strcmp(media, "pcm")) { signal_call("call_reject", id, "unsupported_media"); goto done; }
    const char *provider = field(root, "provider");
    if (!provider[0]) provider = "livekit"; /* Compatibility with existing web calls. */
    if (esp_timer_get_time() - item->received > 30000000) { signal_call("call_reject", id, "expired"); goto done; }
    /*
     * 通话拥有麦克风、扬声器和摄像头的最高优先级。
     * 先结束 AI 会话，再停止统一语音采集任务，避免 AI 音频帧与通话媒体混流。
     */
    voice_ai_service_stop_session("视频通话抢占音频资源");
    strlcpy(s_call_id, id, sizeof(s_call_id));
    s_active = true; s_rtc = rtc; s_accepted = false;
    s_deadline = item->received + 35000000;
    s_voice_stopped = true;
    if (voice_service_stop() != ESP_OK) { finish("audio_busy", true); goto done; }
    if (rtc) {
        esp_err_t error = call_media_connect(provider, (char *)item->data);
        if (error != ESP_OK) finish(error == ESP_ERR_NOT_SUPPORTED ? "unsupported_provider" : "media_failed", true);
    } else {
        if (voice_board_audio_set_output_enabled(true) != ESP_OK) { finish("audio_failed", true); goto done; }
        s_accepted = true; signal_call("call_accept", s_call_id, NULL);
    }
done:
    cJSON_Delete(root);
}

static void worker(void *arg)
{
    (void)arg;
    bool was_ready = false;
    for (;;) {
        message_t item;
        if (xQueueReceive(s_queue, &item, s_active && !s_rtc ? 0 : pdMS_TO_TICKS(20)) == pdTRUE) {
            process(&item);
            memset(item.data, 0, item.length); free(item.data);
        }
        if (atomic_exchange(&s_overflow, false)) finish("queue_overflow", true);
        cloud_gateway_status_t status = {0};
        bool ready = cloud_gateway_get_status(&status) == ESP_OK && status.state == CLOUD_GATEWAY_READY;
        if (!ready) { if (s_active) finish("gateway_disconnected", false); }
        else if (!was_ready) capabilities();
        was_ready = ready;
        if (!s_active) {
            if (s_voice_resume_pending && esp_timer_get_time() >= s_resume_deadline) resume_voice();
            continue;
        }
        int64_t now = esp_timer_get_time();
        if (!s_accepted && now >= s_deadline) { finish("timeout", true); continue; }
        if (s_rtc) {
            call_media_state_t state = call_media_state();
            if (call_media_failed() || state == CALL_MEDIA_FAILED) { finish("media_failed", true); continue; }
            if (state == CALL_MEDIA_CONNECTED) {
                s_reconnect_deadline = 0;
                if (!s_accepted && call_media_started()) { s_accepted = true; signal_call("call_accept", s_call_id, NULL); }
            } else if (s_accepted) {
                if (!s_reconnect_deadline) s_reconnect_deadline = now + 15000000;
                if (state == CALL_MEDIA_DISCONNECTED || now >= s_reconnect_deadline) finish("media_disconnected", true);
            }
        } else if (s_accepted) {
            int16_t pcm[320];
            if (voice_board_audio_read(pcm, 320) != ESP_OK) finish("audio_failed", true);
            else (void)s_send(true, pcm, sizeof(pcm));
        }
    }
}

esp_err_t call_service_init(call_service_send_t send)
{
    if (s_queue || !send) return ESP_ERR_INVALID_STATE;
    s_send = send;
    /* Probe before voice starts; a missing camera leaves PCM/voice usable. */
    s_camera_ready = call_media_init() == ESP_OK;
    s_queue = xQueueCreate(12, sizeof(message_t));
    if (!s_queue) return ESP_ERR_NO_MEM;
    if (xTaskCreate(worker, "call_service", 8192, NULL, 6, NULL) != pdPASS) {
        vQueueDelete(s_queue); s_queue = NULL; return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void call_service_on_cloud_message(bool binary, const void *data, size_t length)
{
    if (!s_queue || !data || !length || length > (binary ? 4096 : CLOUD_GATEWAY_MAX_MESSAGE)) return;
    /* 通话未建立时不接收 AI 下行 PCM；通话建立后只接收 PCM 通话媒体。 */
    if (binary && (!s_active || s_rtc)) return;
    /* 文本消息先按协议类型过滤，禁止 AI 内部事件进入通话队列。 */
    if (!binary && !is_call_text_message(data, length)) return;
    message_t item = { .length = length, .binary = binary,
        .data = calloc(1, length + 1), .received = esp_timer_get_time() };
    if (!item.data) { atomic_store(&s_overflow, true); return; }
    memcpy(item.data, data, length);
    if (xQueueSend(s_queue, &item, 0) != pdTRUE) {
        memset(item.data, 0, length); free(item.data); atomic_store(&s_overflow, true);
    }
}
