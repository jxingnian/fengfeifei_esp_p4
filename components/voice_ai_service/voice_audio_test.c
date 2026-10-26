#include "voice_ai_service.h"
#include "voice_board_audio.h"
#include "encoder/impl/esp_opus_enc.h"
#include "decoder/impl/esp_opus_dec.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <stdlib.h>

typedef struct { TaskHandle_t caller; esp_err_t result; } test_context_t;
static void audio_test_task(void *arg)
{
    test_context_t *ctx = arg;
    void *encoder = NULL, *decoder = NULL;
    int16_t *pcm = calloc(960, sizeof(int16_t));
    uint8_t *packet = malloc(4096), *decoded = malloc(8192);
    esp_err_t error = ESP_ERR_NO_MEM;
    if (!pcm || !packet || !decoded) goto done;
    /* 400Hz triangular tone, peak 1500/32768, twelve 60ms frames. */
    for (unsigned i = 0; i < 960; ++i) {
        int phase = i % 40;
        pcm[i] = phase < 20 ? phase * 150 - 1500 : 4500 - phase * 150;
    }
    error = voice_board_audio_set_output_enabled(true);
    for (unsigned i = 0; i < 12 && error == ESP_OK; ++i) error = voice_board_audio_write(pcm, 960);
    if (error == ESP_OK) error = voice_board_audio_drain();
    (void)voice_board_audio_set_output_enabled(false);
    ESP_LOGI("voice_audio_test", "PCM tone: %s", esp_err_to_name(error));
    if (error != ESP_OK) goto done;
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_opus_enc_config_t enc = ESP_OPUS_ENC_CONFIG_DEFAULT();
    enc.sample_rate = 16000; enc.channel = 1; enc.bits_per_sample = 16;
    enc.bitrate = 24000; enc.complexity = 5; enc.frame_duration = ESP_OPUS_ENC_FRAME_DURATION_60_MS;
    enc.application_mode = ESP_OPUS_ENC_APPLICATION_VOIP;
    esp_opus_dec_cfg_t dec = ESP_OPUS_DEC_CONFIG_DEFAULT();
    dec.sample_rate = 16000; dec.channel = 1; dec.frame_duration = ESP_OPUS_DEC_FRAME_DURATION_60_MS;
    dec.self_delimited = false;
    if (esp_opus_enc_open(&enc, sizeof(enc), &encoder) != ESP_AUDIO_ERR_OK ||
        esp_opus_dec_open(&dec, sizeof(dec), &decoder) != ESP_AUDIO_ERR_OK) { error = ESP_FAIL; goto done; }
    error = voice_board_audio_set_output_enabled(true);
    for (unsigned i = 0; i < 12 && error == ESP_OK; ++i) {
        esp_audio_enc_in_frame_t in = { .buffer = (uint8_t *)pcm, .len = 1920 };
        esp_audio_enc_out_frame_t out = { .buffer = packet, .len = 4096 };
        if (esp_opus_enc_process(encoder, &in, &out) != ESP_AUDIO_ERR_OK || !out.encoded_bytes) { error = ESP_FAIL; break; }
        esp_audio_dec_in_raw_t raw = { .buffer = packet, .len = out.encoded_bytes };
        esp_audio_dec_out_frame_t frame = { .buffer = decoded, .len = 8192 };
        esp_audio_dec_info_t info = {0};
        if (esp_opus_dec_decode(decoder, &raw, &frame, &info) != ESP_AUDIO_ERR_OK ||
            raw.consumed != raw.len || frame.decoded_size != 1920) { error = ESP_FAIL; break; }
        error = voice_board_audio_write((int16_t *)decoded, frame.decoded_size / 2);
    }
    if (error == ESP_OK) error = voice_board_audio_drain();
    ESP_LOGI("voice_audio_test", "Opus tone: %s; stack free=%u bytes", esp_err_to_name(error), (unsigned)uxTaskGetStackHighWaterMark(NULL));
done:
    (void)voice_board_audio_set_output_enabled(false);
    if (encoder) esp_opus_enc_close(encoder);
    if (decoder) esp_opus_dec_close(decoder);
    free(pcm); free(packet); free(decoded);
    ctx->result = error;
    xTaskNotifyGive(ctx->caller);
    vTaskDelete(NULL);
}
esp_err_t voice_ai_service_audio_self_test(void)
{
    /* Startup-only: caller must own the idle hardware, before microphone/RTC. */
    test_context_t ctx = { .caller = xTaskGetCurrentTaskHandle(), .result = ESP_FAIL };
    if (xTaskCreate(audio_test_task, "voice_test", 32 * 1024, &ctx, 4, NULL) != pdPASS) return ESP_ERR_NO_MEM;
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    return ctx.result;
}
