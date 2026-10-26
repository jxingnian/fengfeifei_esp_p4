/*
 * @file voice_board_audio.c
 * @brief ESP32-P4-WIFI6-DB 最小 ES8311 音频驱动。
 *
 * 硬件连接依据开发板 Demo：
 * - I2C SDA：GPIO7
 * - I2C SCL：GPIO8
 * - I2S MCLK：GPIO13
 * - I2S BCLK：GPIO12
 * - I2S WS：GPIO10
 * - I2S DOUT：GPIO9
 * - I2S DIN：GPIO11
 * - 功放使能：GPIO53
 *
 * ES8311 配置为从模式，模拟麦克风通过 ADC 输出到 I2S RX。
 */
#include "voice_board_audio.h"

#include <stdbool.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifndef CONFIG_VOICE_SERVICE_MIC_GAIN_DB
#define CONFIG_VOICE_SERVICE_MIC_GAIN_DB 24
#endif

static const char *TAG = "voice_board_audio";

static const i2c_port_t AUDIO_I2C_PORT = I2C_NUM_1;
static const gpio_num_t AUDIO_I2C_SDA = GPIO_NUM_7;
static const gpio_num_t AUDIO_I2C_SCL = GPIO_NUM_8;
static const uint8_t AUDIO_I2S_PORT = I2S_NUM_1;
static const gpio_num_t AUDIO_I2S_MCLK = GPIO_NUM_13;
static const gpio_num_t AUDIO_I2S_BCLK = GPIO_NUM_12;
static const gpio_num_t AUDIO_I2S_WS = GPIO_NUM_10;
static const gpio_num_t AUDIO_I2S_DOUT = GPIO_NUM_9;
static const gpio_num_t AUDIO_I2S_DIN = GPIO_NUM_11;
static const gpio_num_t AUDIO_PA_GPIO = GPIO_NUM_53;
static const uint32_t AUDIO_SAMPLE_RATE_HZ = 16000;

static i2c_master_bus_handle_t s_i2c_bus;
static i2s_chan_handle_t s_i2s_tx;
static i2s_chan_handle_t s_i2s_rx;
static const audio_codec_data_if_t *s_i2s_data_if;
static const audio_codec_gpio_if_t *s_gpio_if;
static const audio_codec_ctrl_if_t *s_ctrl_if;
static const audio_codec_if_t *s_codec_if;
static esp_codec_dev_handle_t s_microphone;
static esp_codec_dev_handle_t s_speaker;
static bool s_initialized;

static esp_err_t restore_output_level(void)
{
    esp_err_t error = esp_codec_dev_set_out_vol(s_speaker, CONFIG_VOICE_AI_SPEAKER_VOLUME);
    if (error == ESP_OK) error = esp_codec_dev_set_out_mute(s_speaker, false);
    return error;
}

/**
 * @brief 释放已经创建的 I2S 和 codec 接口。
 */
static void voice_board_audio_cleanup(void)
{
    if (s_microphone != NULL) {
        (void)esp_codec_dev_close(s_microphone);
        esp_codec_dev_delete(s_microphone);
        s_microphone = NULL;
    }
    if (s_speaker != NULL) {
        (void)esp_codec_dev_close(s_speaker);
        esp_codec_dev_delete(s_speaker);
        s_speaker = NULL;
    }

    if (s_codec_if != NULL) {
        audio_codec_delete_codec_if(s_codec_if);
        s_codec_if = NULL;
    }
    if (s_ctrl_if != NULL) {
        audio_codec_delete_ctrl_if(s_ctrl_if);
        s_ctrl_if = NULL;
    }
    if (s_gpio_if != NULL) {
        audio_codec_delete_gpio_if(s_gpio_if);
        s_gpio_if = NULL;
    }

    if (s_i2s_data_if != NULL) {
        audio_codec_delete_data_if(s_i2s_data_if);
        s_i2s_data_if = NULL;
    }

    if (s_i2s_tx != NULL) {
        (void)i2s_channel_disable(s_i2s_tx);
        (void)i2s_del_channel(s_i2s_tx);
        s_i2s_tx = NULL;
    }
    if (s_i2s_rx != NULL) {
        (void)i2s_channel_disable(s_i2s_rx);
        (void)i2s_del_channel(s_i2s_rx);
        s_i2s_rx = NULL;
    }

    if (s_i2c_bus != NULL) {
        (void)i2c_del_master_bus(s_i2c_bus);
        s_i2c_bus = NULL;
    }

    s_initialized = false;
}

/**
 * @brief 创建板级 I2C 总线。
 */
static esp_err_t voice_board_audio_init_i2c(void)
{
    const i2c_master_bus_config_t config = {
        .i2c_port = AUDIO_I2C_PORT,
        .sda_io_num = AUDIO_I2C_SDA,
        .scl_io_num = AUDIO_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
    };
    return i2c_new_master_bus(&config, &s_i2c_bus);
}

/**
 * @brief 创建 16 kHz、16 bit、单声道 I2S 双工通道。
 */
static esp_err_t voice_board_audio_init_i2s(void)
{
    i2s_chan_config_t channel_config =
        I2S_CHANNEL_DEFAULT_CONFIG(AUDIO_I2S_PORT, I2S_ROLE_MASTER);
    channel_config.auto_clear = true;
    channel_config.dma_desc_num = 6;
    channel_config.dma_frame_num = 160;

    esp_err_t error = i2s_new_channel(
        &channel_config,
        &s_i2s_tx,
        &s_i2s_rx
    );
    if (error != ESP_OK) {
        return error;
    }

    const i2s_std_config_t standard_config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT,
            I2S_SLOT_MODE_MONO
        ),
        .gpio_cfg = {
            .mclk = AUDIO_I2S_MCLK,
            .bclk = AUDIO_I2S_BCLK,
            .ws = AUDIO_I2S_WS,
            .dout = AUDIO_I2S_DOUT,
            .din = AUDIO_I2S_DIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    error = i2s_channel_init_std_mode(s_i2s_tx, &standard_config);
    if (error != ESP_OK) {
        return error;
    }
    error = i2s_channel_init_std_mode(s_i2s_rx, &standard_config);
    if (error != ESP_OK) {
        return error;
    }
    error = i2s_channel_enable(s_i2s_tx);
    if (error != ESP_OK) {
        return error;
    }
    return i2s_channel_enable(s_i2s_rx);
}

/**
 * @brief 创建 ES8311 输入 codec 设备。
 */
static esp_err_t voice_board_audio_init_codec(void)
{
    audio_codec_i2s_cfg_t i2s_config = {
        .port = AUDIO_I2S_PORT,
        .rx_handle = s_i2s_rx,
        .tx_handle = s_i2s_tx,
    };
    s_i2s_data_if = audio_codec_new_i2s_data(&i2s_config);
    if (s_i2s_data_if == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_gpio_if = audio_codec_new_gpio();
    if (s_gpio_if == NULL) {
        return ESP_ERR_NO_MEM;
    }

    audio_codec_i2c_cfg_t i2c_config = {
        .port = AUDIO_I2C_PORT,
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = s_i2c_bus,
    };
    s_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_config);
    if (s_ctrl_if == NULL) {
        audio_codec_delete_gpio_if(s_gpio_if);
        s_gpio_if = NULL;
        return ESP_ERR_NO_MEM;
    }

    es8311_codec_cfg_t codec_config = {
        .ctrl_if = s_ctrl_if,
        .gpio_if = s_gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
        .pa_pin = AUDIO_PA_GPIO,
        .pa_reverted = false,
        .master_mode = false,
        .use_mclk = true,
        .digital_mic = false,
        .invert_mclk = false,
        .invert_sclk = false,
        .no_dac_ref = true,
        .hw_gain = {
            .pa_voltage = 5.0,
            .codec_dac_voltage = 3.3,
        },
        .mclk_div = 256,
    };
    s_codec_if = es8311_codec_new(&codec_config);
    if (s_codec_if == NULL) {
        audio_codec_delete_ctrl_if(s_ctrl_if);
        audio_codec_delete_gpio_if(s_gpio_if);
        s_ctrl_if = NULL;
        s_gpio_if = NULL;
        return ESP_FAIL;
    }

    esp_codec_dev_cfg_t device_config = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = s_codec_if,
        .data_if = s_i2s_data_if,
    };
    s_microphone = esp_codec_dev_new(&device_config);
    if (s_microphone == NULL) {
        audio_codec_delete_codec_if(s_codec_if);
        audio_codec_delete_ctrl_if(s_ctrl_if);
        audio_codec_delete_gpio_if(s_gpio_if);
        s_codec_if = NULL;
        s_ctrl_if = NULL;
        s_gpio_if = NULL;
        return ESP_ERR_NO_MEM;
    }

    device_config.dev_type = ESP_CODEC_DEV_TYPE_OUT;
    s_speaker = esp_codec_dev_new(&device_config);
    if (s_speaker == NULL) return ESP_ERR_NO_MEM;

    esp_codec_dev_sample_info_t sample_info = {
        .sample_rate = AUDIO_SAMPLE_RATE_HZ,
        .channel = I2S_SLOT_MODE_MONO,
        .bits_per_sample = 16,
    };
    esp_err_t error = esp_codec_dev_open(s_microphone, &sample_info);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "打开 ES8311 麦克风失败：%s", esp_err_to_name(error));
        return error;
    }
    error = esp_codec_dev_open(s_speaker, &sample_info);
    if (error != ESP_OK) return error;
    error = restore_output_level();
    if (error != ESP_OK) return error;
    ESP_LOGI(TAG, "ES8311 speaker volume=%d, unmuted", CONFIG_VOICE_AI_SPEAKER_VOLUME);

    /*
     * 官方开发板录音示例提供 24 dB 麦克风增益选项。这里在设备打开后
     * 立即应用同一增益，保证 MultiNet 接收到稳定的语音幅度。
     */
    error = esp_codec_dev_set_in_gain(s_microphone, CONFIG_VOICE_SERVICE_MIC_GAIN_DB);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "设置 ES8311 麦克风增益失败：%s", esp_err_to_name(error));
        return error;
    }

    ESP_LOGI(TAG, "ES8311 麦克风增益已设置为 %d dB", CONFIG_VOICE_SERVICE_MIC_GAIN_DB);
    return ESP_OK;
}

esp_err_t voice_board_audio_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    esp_err_t error = gpio_set_direction(AUDIO_PA_GPIO, GPIO_MODE_OUTPUT);
    if (error != ESP_OK) {
        return error;
    }
    error = gpio_set_level(AUDIO_PA_GPIO, 0);
    if (error != ESP_OK) {
        return error;
    }
    error = voice_board_audio_init_i2c();
    if (error == ESP_OK) {
        error = voice_board_audio_init_i2s();
    }
    if (error == ESP_OK) {
        error = voice_board_audio_init_codec();
    }
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "初始化 ES8311 音频链路失败：%s", esp_err_to_name(error));
        voice_board_audio_cleanup();
        return error;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "ES8311 麦克风已启动：16 kHz、16 bit、单声道");
    return ESP_OK;
}

esp_err_t voice_board_audio_read(int16_t *samples, size_t sample_count)
{
    if (!s_initialized || s_microphone == NULL || samples == NULL || sample_count == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_codec_dev_read(s_microphone, samples, sample_count * sizeof(int16_t));
}

esp_err_t voice_board_audio_deinit(void)
{
    voice_board_audio_cleanup();
    return ESP_OK;
}

esp_err_t voice_board_audio_write(const int16_t *samples, size_t sample_count)
{
    if (!s_initialized || s_speaker == NULL || samples == NULL || sample_count == 0) return ESP_ERR_INVALID_STATE;
    return esp_codec_dev_write(s_speaker, (void *)samples, sample_count * sizeof(int16_t));
}

esp_err_t voice_board_audio_set_output_enabled(bool enabled)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (enabled) {
        esp_err_t error = restore_output_level();
        if (error != ESP_OK) return error;
    }
    return gpio_set_level(AUDIO_PA_GPIO, enabled ? 1 : 0);
}

esp_err_t voice_board_audio_drain(void)
{
    /* Six 160-sample DMA descriptors = 60ms; push silence through the whole
     * ring then wait one more ring before disabling PA. Caller owns output. */
    static const int16_t silence[960] = {0};
    esp_err_t error = voice_board_audio_write(silence, 960);
    if (error == ESP_OK) vTaskDelay(pdMS_TO_TICKS(70));
    return error;
}

i2c_master_bus_handle_t voice_board_audio_i2c_bus(void) { return s_i2c_bus; }
esp_codec_dev_handle_t voice_board_audio_microphone(void) { return s_microphone; }
esp_codec_dev_handle_t voice_board_audio_speaker(void) { return s_speaker; }

esp_err_t voice_board_audio_restore(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    (void)esp_codec_dev_close(s_microphone);
    (void)esp_codec_dev_close(s_speaker);
    esp_codec_dev_sample_info_t info = {
        .sample_rate = 16000, .channel = 1, .bits_per_sample = 16,
    };
    esp_err_t error = esp_codec_dev_open(s_microphone, &info);
    if (error == ESP_OK) error = esp_codec_dev_open(s_speaker, &info);
    if (error == ESP_OK) error = esp_codec_dev_set_in_gain(s_microphone, CONFIG_VOICE_SERVICE_MIC_GAIN_DB);
    if (error == ESP_OK) error = restore_output_level();
    (void)voice_board_audio_set_output_enabled(false);
    return error;
}
