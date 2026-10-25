#include "call_service.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "voice_board_audio.h"
#include "cloud_gateway.h"
#include "voice_service.h"

typedef struct { size_t length; uint8_t *data; } audio_item_t;
static QueueHandle_t s_audio_queue;
static call_service_send_t s_send;
static volatile bool s_active;
static char s_call_id[80];

static void send_json(const char *type)
{
    char message[160];
    snprintf(message, sizeof(message), "{\"type\":\"%s\",\"callId\":\"%s\"}", type, s_call_id);
    if (s_send) (void)s_send(false, message, strlen(message));
}

static void call_audio_task(void *arg)
{
    (void)arg;
    int16_t samples[320];
    while (true) {
        audio_item_t item;
        while (xQueueReceive(s_audio_queue, &item, 0) == pdTRUE) {
            if (s_active) (void)voice_board_audio_write((const int16_t *)item.data, item.length / sizeof(int16_t));
            free(item.data);
        }
        if (!s_active) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
        if (voice_board_audio_read(samples, 320) == ESP_OK && s_send) {
            (void)s_send(true, samples, sizeof(samples));
        }
    }
}

esp_err_t call_service_init(call_service_send_t send)
{
    if (s_audio_queue) return ESP_ERR_INVALID_STATE;
    s_send = send;
    s_audio_queue = xQueueCreate(8, sizeof(audio_item_t));
    if (!s_audio_queue) return ESP_ERR_NO_MEM;
    return xTaskCreate(call_audio_task, "call_audio", 4096, NULL, 6, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

void call_service_on_cloud_message(bool binary, const void *data, size_t length)
{
    if (binary) {
        if (!s_active || length == 0 || length > 4096) return;
        audio_item_t item = { .length = length, .data = malloc(length) };
        if (!item.data) return;
        memcpy(item.data, data, length);
        if (xQueueSend(s_audio_queue, &item, 0) != pdTRUE) free(item.data);
        return;
    }
    if (!data || !length || length > 512) return;
    char *text = calloc(1, length + 1);
    if (!text) return;
    memcpy(text, data, length);
    cJSON *root = cJSON_Parse(text);
    free(text);
    if (!root) return;
    const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(root, "type"));
    const char *call_id = cJSON_GetStringValue(cJSON_GetObjectItem(root, "callId"));
    if (type && !strcmp(type, "call_offer") && call_id) {
        strlcpy(s_call_id, call_id, sizeof(s_call_id));
        (void)voice_service_stop();
        (void)voice_board_audio_set_output_enabled(true);
        s_active = true;
        send_json("call_accept");
    } else if (type && !strcmp(type, "call_end")) {
        s_active = false;
        (void)voice_board_audio_set_output_enabled(false);
        (void)voice_service_start();
        s_call_id[0] = 0;
    }
    cJSON_Delete(root);
}
