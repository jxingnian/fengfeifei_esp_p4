#include "call_media_provider.h"
#include "livekit.h"
#include "cJSON.h"
#include <string.h>
#include <stdlib.h>
#include <stdatomic.h>
#include "sdkconfig.h"
#include "voice_board_audio.h"
#include "esp_audio_enc_default.h"
#include "esp_audio_dec_default.h"
#include "esp_video_enc_default.h"
#include "esp_capture_defaults.h"
#include "esp_video_init.h"
#include "av_render_default.h"
#include "esp_log.h"

static const char *TAG = "call_media";
static bool s_system_ready, s_camera_ready;
static esp_capture_handle_t s_capture;
static esp_capture_audio_src_if_t *s_audio;
static esp_capture_video_src_if_t *s_video;
static audio_render_handle_t s_audio_render;
static av_render_handle_t s_render;
static livekit_room_handle_t s_room;
static atomic_bool s_audio_started, s_video_started, s_failed;

static esp_capture_err_t capture_event(esp_capture_event_t event, void *ctx)
{
    (void)ctx;
    if (event == ESP_CAPTURE_EVENT_FIRST_AUDIO_FRAME) atomic_store(&s_audio_started, true);
    if (event == ESP_CAPTURE_EVENT_FIRST_VIDEO_FRAME) atomic_store(&s_video_started, true);
    if (event == ESP_CAPTURE_EVENT_ERROR) atomic_store(&s_failed, true);
    return ESP_CAPTURE_ERR_OK;
}

static esp_err_t init(void)
{
    if (!s_system_ready) {
        if (livekit_system_init() != LIVEKIT_ERR_NONE) return ESP_FAIL;
        esp_audio_enc_register_default();
        esp_audio_dec_register_default();
        esp_video_enc_register_default();
        s_system_ready = true;
    }
    if (s_camera_ready) return ESP_OK;
    if (!voice_board_audio_i2c_bus()) return ESP_ERR_INVALID_STATE;
    /* Manufacturer BSP: shared I2C1 GPIO7/8, no reset/power GPIO.
     * esp_video owns CSI LDO3 (2.5 V). No display initialization. */
    esp_video_init_csi_config_t csi = {
        .sccb_config = { .init_sccb = false,
            .i2c_handle = voice_board_audio_i2c_bus(), .freq = 400000 },
        .reset_pin = -1, .pwdn_pin = -1,
    };
    esp_video_init_config_t config = { .csi = &csi };
    esp_err_t error = esp_video_init(&config);
    if (error != ESP_OK) {
        (void)esp_video_deinit();
        ESP_LOGE(TAG, "CSI sensor initialization failed: %s", esp_err_to_name(error));
        return error;
    }
    esp_capture_video_v4l2_src_cfg_t probe_cfg = { .dev_name = "/dev/video0", .buf_count = 2 };
    esp_capture_video_src_if_t *probe = esp_capture_new_video_v4l2_src(&probe_cfg);
    if (!probe || probe->open(probe) != ESP_CAPTURE_ERR_OK) {
        if (probe) { (void)probe->close(probe); free(probe); }
        (void)esp_video_deinit();
        ESP_LOGE(TAG, "Camera V4L2 probe failed");
        return ESP_ERR_NOT_FOUND;
    }
    (void)probe->close(probe);
    free(probe);
    s_camera_ready = true;
    return ESP_OK;
}

static void close_media(void);

static esp_err_t connect_media(const char *url, const char *token)
{
    if (s_room) return ESP_ERR_INVALID_STATE;
    esp_err_t error = init();
    if (error != ESP_OK) return error;
    atomic_store(&s_audio_started, false);
    atomic_store(&s_video_started, false);
    atomic_store(&s_failed, false);
    esp_capture_audio_dev_src_cfg_t audio = { .record_handle = voice_board_audio_microphone() };
    esp_capture_video_v4l2_src_cfg_t video = { .dev_name = "/dev/video0", .buf_count = 2 };
    s_audio = esp_capture_new_audio_dev_src(&audio);
    s_video = esp_capture_new_video_v4l2_src(&video);
    if (!s_audio || !s_video) goto fail;
    esp_capture_cfg_t capture = { .sync_mode = ESP_CAPTURE_SYNC_MODE_AUDIO,
        .audio_src = s_audio, .video_src = s_video };
    if (esp_capture_open(&capture, &s_capture) != ESP_CAPTURE_ERR_OK) goto fail;
    if (esp_capture_set_event_cb(s_capture, capture_event, NULL) != ESP_CAPTURE_ERR_OK) goto fail;
    i2s_render_cfg_t i2s = { .play_handle = voice_board_audio_speaker(), .fixed_clock = true };
    s_audio_render = av_render_alloc_i2s_render(&i2s);
    if (!s_audio_render) goto fail;
    av_render_cfg_t render = { .audio_render = s_audio_render,
        .audio_raw_fifo_size = 8 * 4096, .audio_render_fifo_size = 100 * 1024,
        .allow_drop_data = false };
    s_render = av_render_open(&render);
    if (!s_render) goto fail;
    av_render_audio_frame_info_t frame = { .sample_rate = 16000, .channel = 1, .bits_per_sample = 16 };
    if (av_render_set_fixed_frame_info(s_render, &frame) != 0) goto fail;
    if (esp_codec_dev_set_out_vol(i2s.play_handle, CONFIG_CALL_SPEAKER_VOLUME) != ESP_OK) goto fail;
    if (voice_board_audio_set_output_enabled(true) != ESP_OK) goto fail;
    livekit_room_options_t options = {
        .publish = { .kind = LIVEKIT_MEDIA_TYPE_BOTH,
            .audio_encode = { .codec = LIVEKIT_AUDIO_CODEC_OPUS, .sample_rate = 16000, .channel_count = 1 },
            .video_encode = { .codec = LIVEKIT_VIDEO_CODEC_H264,
                .width = CONFIG_CALL_VIDEO_WIDTH, .height = CONFIG_CALL_VIDEO_HEIGHT, .fps = CONFIG_CALL_VIDEO_FPS },
            .capturer = s_capture },
        .subscribe = { .kind = LIVEKIT_MEDIA_TYPE_AUDIO, .renderer = s_render },
    };
    if (livekit_room_create(&s_room, &options) != LIVEKIT_ERR_NONE) goto fail;
    if (livekit_room_connect(s_room, url, token) != LIVEKIT_ERR_NONE) goto fail;
    return ESP_OK;
fail:
    close_media();
    return ESP_FAIL;
}

static call_media_state_t state(void)
{
    if (!s_room) return CALL_MEDIA_DISCONNECTED;
    switch (livekit_room_get_state(s_room)) {
    case LIVEKIT_CONNECTION_STATE_CONNECTED: return CALL_MEDIA_CONNECTED;
    case LIVEKIT_CONNECTION_STATE_FAILED: return CALL_MEDIA_FAILED;
    case LIVEKIT_CONNECTION_STATE_DISCONNECTED: return CALL_MEDIA_DISCONNECTED;
    default: return CALL_MEDIA_CONNECTING;
    }
}
static bool started(void) { return atomic_load(&s_audio_started) && atomic_load(&s_video_started); }
static bool failed(void) { return atomic_load(&s_failed); }

static void close_media(void)
{
    if (s_room) {
        (void)livekit_room_close(s_room);
        /* destroy joins the engine before the capture/renderer are freed. */
        (void)livekit_room_destroy(s_room);
        s_room = NULL;
    }
    if (s_capture) { (void)esp_capture_close(s_capture); s_capture = NULL; }
    /* Source interfaces are allocated separately and are caller-owned. */
    if (s_audio && s_audio->close) (void)s_audio->close(s_audio);
    if (s_video && s_video->close) (void)s_video->close(s_video);
    free(s_audio); s_audio = NULL;
    free(s_video); s_video = NULL;
    if (s_render) { (void)av_render_close(s_render); s_render = NULL; }
    if (s_audio_render) { audio_render_free_handle(s_audio_render); s_audio_render = NULL; }
    (void)voice_board_audio_set_output_enabled(false);
    atomic_store(&s_audio_started, false);
    atomic_store(&s_video_started, false);
}

static esp_err_t connect_offer(const char *offer_json)
{
    cJSON *root = cJSON_Parse(offer_json);
    if (!root) return ESP_ERR_INVALID_ARG;
    const char *url = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "rtcUrl"));
    const char *token = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "token"));
    esp_err_t error = ESP_ERR_INVALID_ARG;
    if (url && token && (!strncmp(url, "wss://", 6) || !strncmp(url, "ws://", 5)) &&
        strlen(url) <= 512 && token[0] && strlen(token) <= 8192) error = connect_media(url, token);
    cJSON_Delete(root);
    return error;
}

const call_media_provider_t call_media_livekit_provider = {
    .id = "livekit", .init = init, .connect_offer = connect_offer, .state = state,
    .started = started, .failed = failed, .close = close_media,
};
