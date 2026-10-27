/*
 * @file wake_word.c
 * @brief 基于 ES8311 和中文 MultiNet 的“你好小飞”语音唤醒实现。
 *
 * 实现原则：
 * 1. 只在本组件内访问麦克风 codec 和 ESP-SR，业务层只接收事件。
 * 2. 使用 ESP-SR 官方中文 MultiNet7 模型，不伪造 WakeNet 内置模型。
 * 3. 通过拼音命令“ni hao xiao fei”注册中文命令词。
 * 4. 使用单独 FreeRTOS 任务读取 16 kHz、16 bit、单声道 PCM 并识别。
 * 5. 初始化失败时明确上报错误，禁止打印启动成功后实际没有识别任务。
 */
#include "wake_word.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdatomic.h>

#include "esp_log.h"
#include "esp_mn_iface.h"
#include "esp_mn_models.h"
#include "esp_mn_speech_commands.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "model_path.h"
#include "audio_service.h"

#ifndef CONFIG_WAKE_WORD_MN_THRESHOLD
#define CONFIG_WAKE_WORD_MN_THRESHOLD 0.60
#endif

#ifndef CONFIG_WAKE_WORD_COMMAND_TIMEOUT_MS
#define CONFIG_WAKE_WORD_COMMAND_TIMEOUT_MS 6000
#endif

static const char *TAG = "wake_word";
static const char *WAKE_WORD_PHRASE = "ni hao xiao fei";
static const int WAKE_WORD_COMMAND_ID = 1;
static srmodel_list_t *s_models;
static esp_mn_iface_t *s_multinet;
static model_iface_data_t *s_multinet_data;
static bool s_commands_allocated;
static TaskHandle_t s_recognition_task;
static atomic_bool s_running;
static atomic_bool s_conversation_active;
static _Atomic wake_word_state_t s_state = WAKE_WORD_STATE_STOPPED;
static wake_word_event_callback_t s_callback;
static void *s_callback_context;
static wake_word_pcm_callback_t s_pcm_callback;
static void *s_pcm_callback_context;

/**
 * @brief 向业务层发送事件。
 */
static void wake_word_emit_event(
    wake_word_event_type_t type,
    int command_id,
    float probability,
    esp_err_t error
)
{
    wake_word_event_t event = {
        .type = type,
        .state = s_state,
        .command_id = command_id,
        .probability = probability,
        .error = error,
    };

    if (s_callback != NULL) {
        s_callback(&event, s_callback_context);
    }
}

/**
 * @brief 释放已经创建的模型和麦克风资源。
 */
static void wake_word_release_resources(void)
{
    /*
     * MultiNet 命令表内部保存模型接口和实例指针，因此必须先释放命令表，
     * 再销毁模型实例，避免命令管理模块继续持有已经失效的对象地址。
     */
    if (s_commands_allocated) {
        (void)esp_mn_commands_free();
        s_commands_allocated = false;
    }

    if (s_multinet != NULL && s_multinet_data != NULL) {
        s_multinet->destroy(s_multinet_data);
    }
    s_multinet_data = NULL;
    s_multinet = NULL;

    if (s_models != NULL) {
        esp_srmodel_deinit(s_models);
    }
    s_models = NULL;

    /* audio_service 的生命周期由 app_main 统一管理，唤醒词释放模型时不能
     * 销毁共享 ES8311；这里只释放本组件持有的音频所有权。 */
    (void)audio_service_stop_capture(
        s_conversation_active
            ? AUDIO_SERVICE_OWNER_AI_DIALOG
            : AUDIO_SERVICE_OWNER_WAKE_WORD
    );
    if (!s_conversation_active) {
        (void)audio_service_release(AUDIO_SERVICE_OWNER_WAKE_WORD);
    }
}

/**
 * @brief 初始化 ES8311 麦克风并设置为 16 kHz、16 bit、单声道。
 */
static esp_err_t wake_word_init_microphone(void)
{
    esp_err_t error = audio_service_acquire(
        AUDIO_SERVICE_OWNER_WAKE_WORD,
        portMAX_DELAY
    );
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "初始化 ES8311 板级音频失败：%s", esp_err_to_name(error));
        return error;
    }

    return ESP_OK;
}

/**
 * @brief 初始化中文 MultiNet7 并注册“你好小飞”命令词。
 */
static esp_err_t wake_word_init_multinet(void)
{
    s_models = esp_srmodel_init("model");
    if (s_models == NULL) {
        ESP_LOGE(TAG, "加载 ESP-SR model 分区失败，请确认已烧录 model 分区");
        return ESP_ERR_NOT_FOUND;
    }

    char *model_name = esp_srmodel_filter(s_models, ESP_MN_PREFIX, "mn7_cn");
    if (model_name == NULL) {
        ESP_LOGE(TAG, "未找到中文 MultiNet7 模型 mn7_cn");
        return ESP_ERR_NOT_FOUND;
    }

    s_multinet = esp_mn_handle_from_name(model_name);
    if (s_multinet == NULL) {
        ESP_LOGE(TAG, "无法获取 MultiNet 模型接口：%s", model_name);
        return ESP_FAIL;
    }

    s_multinet_data = s_multinet->create(
        model_name,
        CONFIG_WAKE_WORD_COMMAND_TIMEOUT_MS
    );
    if (s_multinet_data == NULL) {
        ESP_LOGE(TAG, "创建 MultiNet 模型实例失败：%s", model_name);
        return ESP_ERR_NO_MEM;
    }

    /*
     * ESP-SR 动态命令接口使用全局命令链表。首次添加命令前必须绑定
     * 当前 MultiNet 接口和模型实例，否则 add/update 会返回未初始化错误。
     */
    esp_err_t error = esp_mn_commands_alloc(s_multinet, s_multinet_data);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "初始化 MultiNet 命令表失败：%s", esp_err_to_name(error));
        return error;
    }
    s_commands_allocated = true;

    error = esp_mn_commands_add(WAKE_WORD_COMMAND_ID, WAKE_WORD_PHRASE);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "注册命令词 [%s] 失败：%s", WAKE_WORD_PHRASE, esp_err_to_name(error));
        return error;
    }

    esp_mn_error_t *command_error = esp_mn_commands_update();
    if (command_error != NULL && command_error->num > 0) {
        ESP_LOGE(TAG, "命令词 [%s] 未被 MultiNet 接受，错误数量：%d",
                 WAKE_WORD_PHRASE, command_error->num);
        return ESP_ERR_INVALID_ARG;
    }

    error = s_multinet->set_det_threshold(
        s_multinet_data,
        CONFIG_WAKE_WORD_MN_THRESHOLD
    );
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "设置 MultiNet 阈值失败：%s", esp_err_to_name(error));
        return error;
    }

    s_multinet->print_active_speech_commands(s_multinet_data);
    ESP_LOGI(TAG, "中文 MultiNet 已加载，唤醒词：你好小飞（%s）", WAKE_WORD_PHRASE);
    return ESP_OK;
}

/**
 * @brief 读取模型要求的固定数量 PCM 样本。
 */
static esp_err_t wake_word_read_pcm(int16_t *buffer, size_t sample_count)
{
    /* 唤醒回调切换所有权后，同一个采集任务继续读取，但必须以 AI 对话
     * 所有者身份访问服务，避免共享音频服务把这次读取判定为越权。 */
    const audio_service_owner_t owner = s_conversation_active
        ? AUDIO_SERVICE_OWNER_AI_DIALOG
        : AUDIO_SERVICE_OWNER_WAKE_WORD;
    return audio_service_read_pcm(
        owner,
        buffer,
        sample_count
    );
}

/**
 * @brief MultiNet 持续识别任务。
 */
static void wake_word_recognition_task(void *argument)
{
    (void)argument;

    const int sample_chunk_size = s_multinet->get_samp_chunksize(s_multinet_data);
    if (sample_chunk_size <= 0) {
        ESP_LOGE(TAG, "MultiNet 返回非法音频块大小：%d", sample_chunk_size);
        s_state = WAKE_WORD_STATE_ERROR;
        wake_word_emit_event(WAKE_WORD_EVENT_MODEL_ERROR, 0, 0.0f, ESP_ERR_INVALID_SIZE);
        s_running = false;
        s_recognition_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    int16_t *audio_buffer = calloc((size_t)sample_chunk_size, sizeof(int16_t));
    if (audio_buffer == NULL) {
        ESP_LOGE(TAG, "分配 MultiNet 音频缓冲区失败，样本数：%d", sample_chunk_size);
        s_state = WAKE_WORD_STATE_ERROR;
        wake_word_emit_event(WAKE_WORD_EVENT_MODEL_ERROR, 0, 0.0f, ESP_ERR_NO_MEM);
        s_running = false;
        s_recognition_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "语音监听任务已启动，chunk=%d，采样率=%d Hz",
             sample_chunk_size,
             s_multinet->get_samp_rate(s_multinet_data));

    while (s_running) {
        esp_err_t error = wake_word_read_pcm(audio_buffer, (size_t)sample_chunk_size);
        if (error != ESP_OK) {
            ESP_LOGE(TAG, "读取 ES8311 PCM 失败：%s", esp_err_to_name(error));
            s_state = WAKE_WORD_STATE_ERROR;
            wake_word_emit_event(WAKE_WORD_EVENT_AUDIO_ERROR, 0, 0.0f, error);
            s_running = false;
            break;
        }

        /*
         * 唤醒成功后继续复用同一个读取任务，把 PCM 交给 AI 会话服务。
         * 这里不再调用 MultiNet，避免 AI 会话期间第二次读取同一音频设备。
         */
        if (s_conversation_active) {
            if (s_pcm_callback != NULL) {
                s_pcm_callback(audio_buffer, (size_t)sample_chunk_size, s_pcm_callback_context);
            }
            continue;
        }

        esp_mn_state_t state = s_multinet->detect(s_multinet_data, audio_buffer);
        if (state == ESP_MN_STATE_TIMEOUT) {
            /* MultiNet 完成一次识别窗口但没有得到可用命令，继续监听。 */
            wake_word_emit_event(WAKE_WORD_EVENT_TIMEOUT, 0, 0.0f, ESP_OK);
            continue;
        }
        if (state != ESP_MN_STATE_DETECTED) {
            continue;
        }

        esp_mn_results_t *result = s_multinet->get_results(s_multinet_data);
        if (result == NULL || result->num <= 0) {
            wake_word_emit_event(WAKE_WORD_EVENT_TIMEOUT, 0, 0.0f, ESP_OK);
            continue;
        }

        for (int result_index = 0; result_index < result->num; ++result_index) {
            if (result->command_id[result_index] != WAKE_WORD_COMMAND_ID) {
                continue;
            }

            ESP_LOGI(TAG, "检测到唤醒词：你好小飞，概率：%.3f",
                     result->prob[result_index]);
            /* 先通知业务层，业务层调用 wake_word_start_conversation() 时会切换采集模式并转移所有权 */
            wake_word_emit_event(
                WAKE_WORD_EVENT_WAKE_DETECTED,
                result->command_id[result_index],
                result->prob[result_index],
                ESP_OK
            );
            break;
        }
    }

    free(audio_buffer);
    s_recognition_task = NULL;
    if (s_state != WAKE_WORD_STATE_ERROR) {
        s_state = WAKE_WORD_STATE_STOPPED;
        wake_word_emit_event(WAKE_WORD_EVENT_STOPPED, 0, 0.0f, ESP_OK);
    }
    vTaskDelete(NULL);
}

esp_err_t wake_word_register_callback(
    wake_word_event_callback_t callback,
    void *user_context
)
{
    s_callback = callback;
    s_callback_context = user_context;
    return ESP_OK;
}

esp_err_t wake_word_register_pcm_callback(
    wake_word_pcm_callback_t callback,
    void *user_context
)
{
    s_pcm_callback = callback;
    s_pcm_callback_context = user_context;
    return ESP_OK;
}

esp_err_t wake_word_init(void)
{
    if (s_state != WAKE_WORD_STATE_STOPPED) {
        return ESP_ERR_INVALID_STATE;
    }

    s_state = WAKE_WORD_STATE_STARTING;
    s_conversation_active = false;
    esp_err_t error = wake_word_init_microphone();
    if (error == ESP_OK) {
        error = wake_word_init_multinet();
    }
    if (error != ESP_OK) {
        wake_word_release_resources();
        s_state = WAKE_WORD_STATE_ERROR;
        wake_word_emit_event(WAKE_WORD_EVENT_MODEL_ERROR, 0, 0.0f, error);
        return error;
    }

    s_state = WAKE_WORD_STATE_STOPPED;
    wake_word_emit_event(WAKE_WORD_EVENT_READY, 0, 0.0f, ESP_OK);
    return ESP_OK;
}

esp_err_t wake_word_start(void)
{
    if (s_multinet == NULL || s_multinet_data == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_recognition_task != NULL || s_running) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t capture_error = audio_service_start_capture(
        AUDIO_SERVICE_OWNER_WAKE_WORD
    );
    if (capture_error != ESP_OK) {
        return capture_error;
    }

    s_running = true;
    s_state = WAKE_WORD_STATE_LISTENING;
    BaseType_t task_result = xTaskCreate(
        wake_word_recognition_task,
        "wake_word_recognition",
        8192,
        NULL,
        5,
        &s_recognition_task
    );
    if (task_result != pdPASS) {
        s_running = false;
        s_state = WAKE_WORD_STATE_ERROR;
        (void)audio_service_stop_capture(AUDIO_SERVICE_OWNER_WAKE_WORD);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t wake_word_stop(void)
{
    if (!s_running && s_recognition_task == NULL) {
        return ESP_OK;
    }

    s_running = false;
    const int64_t deadline_us = esp_timer_get_time() + 1000000;
    while (s_recognition_task != NULL && esp_timer_get_time() < deadline_us) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (s_recognition_task != NULL) {
        return ESP_ERR_TIMEOUT;
    }

    /* 识别任务已经完全退出，释放当前模式对应的采集标记；唤醒回调之后
     * 所有权已经切换到 AI 对话，因此这里必须使用 AI 所有者校验。 */
    (void)audio_service_stop_capture(
        s_conversation_active
            ? AUDIO_SERVICE_OWNER_AI_DIALOG
            : AUDIO_SERVICE_OWNER_WAKE_WORD
    );

    /*
     * 识别任务退出前会统一发送一次 STOPPED 事件。这里仅等待任务完成，
     * 不再重复发送，避免业务层收到两个相同的停止通知。
     */
    return ESP_OK;
}

void wake_word_start_conversation(void)
{
    s_conversation_active = true;
    (void)audio_service_transfer(
        AUDIO_SERVICE_OWNER_WAKE_WORD,
        AUDIO_SERVICE_OWNER_AI_DIALOG
    );
}

void wake_word_stop_conversation(void)
{
    s_conversation_active = false;
    (void)audio_service_transfer(
        AUDIO_SERVICE_OWNER_AI_DIALOG,
        AUDIO_SERVICE_OWNER_WAKE_WORD
    );
}

bool wake_word_is_conversation_active(void)
{
    return s_conversation_active;
}

wake_word_state_t wake_word_get_state(void)
{
    return s_state;
}
