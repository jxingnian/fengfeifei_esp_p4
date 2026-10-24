#include "wifi_provisioning_internal.h"

#include <inttypes.h>
#include <string.h>

#include "esp_event.h"
#include "esp_hosted.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/task.h"
#include "lwip/ip4_addr.h"
#include "nvs_flash.h"

/** 单个 Wi-Fi 配置最多连续尝试三次，满足产品联网阶段的明确重试要求。 */
#define WIFI_PROVISIONING_CONNECT_RETRY_COUNT 3U

/** 每次连接等待 DHCP 地址的最长时间，超时后切换到本配置的下一次尝试。 */
#define WIFI_PROVISIONING_CONNECT_TIMEOUT_MS 12000U

/** 后台连接任务栈大小，包含 ESP-Hosted 远程 Wi-Fi 调用所需的栈空间。 */
#define WIFI_PROVISIONING_TASK_STACK_SIZE 6144U

/** 后台连接任务优先级，保证配网连接不会阻塞业务任务。 */
#define WIFI_PROVISIONING_TASK_PRIORITY 5U

static const char *TAG = "wifi_provisioning";

wifi_provisioning_context_t g_wifi_provisioning_context;

/**
 * @brief 将状态写入上下文，所有字段更新都在同一个临界区内完成。
 */
static void set_state(wifi_provisioning_state_t state)
{
    wifi_provisioning_context_t *context = &g_wifi_provisioning_context;
    xSemaphoreTake(context->state_mutex, portMAX_DELAY);
    context->status.state = state;
    xSemaphoreGive(context->state_mutex);
}

/**
 * @brief 获取指定 AP 配置的连接阈值。
 *
 * 配网组件不限制扫描结果的认证等级，最终认证由 Wi-Fi 驱动根据目标网络
 * 实际安全方式完成；该配置只指定 STA 接受的最低认证级别。
 */
static wifi_auth_mode_t get_sta_auth_threshold(void)
{
#if CONFIG_WIFI_PROVISIONING_AUTH_OPEN
    return WIFI_AUTH_OPEN;
#elif CONFIG_WIFI_PROVISIONING_AUTH_WPA2_WPA3_PSK
    return WIFI_AUTH_WPA2_WPA3_PSK;
#else
    return WIFI_AUTH_WPA2_PSK;
#endif
}

/**
 * @brief 统一处理 Wi-Fi 和 DHCP 事件。
 *
 * 事件回调只负责更新状态和唤醒连接任务，不在系统事件线程中执行重试循环，
 * 这样可以保证扫描、DHCP、HTTP 服务和 ESP-Hosted RPC 事件继续及时运行。
 */
static void wifi_event_handler(void *handler_arg,
                              esp_event_base_t event_base,
                              int32_t event_id,
                              void *event_data)
{
    (void)handler_arg;
    wifi_provisioning_context_t *context = &g_wifi_provisioning_context;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        xEventGroupSetBits(context->event_group, WIFI_PROVISIONING_STA_STARTED_BIT);
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *disconnected =
            (const wifi_event_sta_disconnected_t *)event_data;

        ESP_LOGW(TAG,
                 "STA 已断开，原因码=%u",
                 disconnected == NULL ? 0U : disconnected->reason);
        xSemaphoreTake(context->state_mutex, portMAX_DELAY);
        bool was_connected = context->status.sta_connected;
        bool attempt_active = context->connection_attempt_active;
        bool command_pending = context->reconnect_command_pending;
        context->status.sta_connected = false;
        context->status.current_ssid[0] = '\0';
        memset(&context->status.sta_ip_info, 0, sizeof(context->status.sta_ip_info));
        context->status.state = attempt_active ? WIFI_PROVISIONING_STATE_CONNECTING
                                               : WIFI_PROVISIONING_STATE_PROVISIONING;
        if (was_connected && !attempt_active && !command_pending && context->started) {
            context->reconnect_command_pending = true;
        }
        xSemaphoreGive(context->state_mutex);
        xEventGroupClearBits(context->event_group, WIFI_PROVISIONING_CONNECTED_BIT);

        if (attempt_active) {
            xEventGroupSetBits(context->event_group, WIFI_PROVISIONING_ATTEMPT_FAILED_BIT);
        } else if (was_connected && !command_pending) {
            wifi_provisioning_command_t command = {
                .type = WIFI_PROVISIONING_COMMAND_RECONNECT_SAVED,
            };
            if (xQueueSend(context->command_queue, &command, 0) != pdPASS) {
                xSemaphoreTake(context->state_mutex, portMAX_DELAY);
                context->reconnect_command_pending = false;
                xSemaphoreGive(context->state_mutex);
                ESP_LOGE(TAG, "断网重连命令入队失败");
            }
        }
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *got_ip = (const ip_event_got_ip_t *)event_data;
        if (got_ip == NULL) {
            ESP_LOGE(TAG, "收到空的 STA DHCP 地址事件");
            return;
        }

        xSemaphoreTake(context->state_mutex, portMAX_DELAY);
        context->status.sta_connected = true;
        context->status.sta_ip_info = got_ip->ip_info;
        context->status.state = WIFI_PROVISIONING_STATE_CONNECTED;
        xSemaphoreGive(context->state_mutex);

        xEventGroupSetBits(context->event_group,
                           WIFI_PROVISIONING_CONNECTED_BIT |
                               WIFI_PROVISIONING_ATTEMPT_SUCCESS_BIT);
        ESP_LOGI(TAG, "STA 已联网，IP=" IPSTR, IP2STR(&got_ip->ip_info.ip));
    }
}

/**
 * @brief 初始化网络栈和默认事件循环。
 */
static esp_err_t initialize_network_infrastructure(void)
{
    esp_err_t error = esp_netif_init();
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) {
        return error;
    }

    error = esp_event_loop_create_default();
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) {
        return error;
    }
    return ESP_OK;
}

/**
 * @brief 建立 P4 与 C5 之间的 ESP-Hosted SDIO 链路。
 */
static esp_err_t connect_c5_coprocessor(void)
{
    int hosted_result = esp_hosted_init();
    if (hosted_result != ESP_OK) {
        ESP_LOGE(TAG, "ESP-Hosted 初始化失败：%s", esp_err_to_name(hosted_result));
        return (esp_err_t)hosted_result;
    }

    hosted_result = esp_hosted_connect_to_slave();
    if (hosted_result != ESP_OK) {
        ESP_LOGE(TAG, "ESP32-C5 SDIO 链路建立失败：%s", esp_err_to_name(hosted_result));
        return (esp_err_t)hosted_result;
    }
    return ESP_OK;
}

/**
 * @brief 配置 AP 的 SSID、密码和固定的配网信道。
 */
static esp_err_t configure_access_point(void)
{
    wifi_config_t access_point_config = {0};
    strlcpy((char *)access_point_config.ap.ssid,
            CONFIG_WIFI_PROVISIONING_AP_SSID,
            sizeof(access_point_config.ap.ssid));
    strlcpy((char *)access_point_config.ap.password,
            CONFIG_WIFI_PROVISIONING_AP_PASSWORD,
            sizeof(access_point_config.ap.password));
    access_point_config.ap.ssid_len = strlen((char *)access_point_config.ap.ssid);
    access_point_config.ap.channel = CONFIG_WIFI_PROVISIONING_AP_CHANNEL;
    access_point_config.ap.max_connection = CONFIG_WIFI_PROVISIONING_AP_MAX_CONNECTION;
    size_t password_length = strlen((char *)access_point_config.ap.password);
    if (access_point_config.ap.ssid_len == 0U ||
        (password_length > 0U && password_length < 8U)) {
        ESP_LOGE(TAG, "配网热点名称不能为空，非空密码长度必须至少为 8 个字符");
        return ESP_ERR_INVALID_ARG;
    }
    access_point_config.ap.authmode = password_length == 0U ? WIFI_AUTH_OPEN
                                                            : WIFI_AUTH_WPA2_PSK;

    return esp_wifi_set_config(WIFI_IF_AP, &access_point_config);
}

/**
 * @brief 将指定配置设置为 STA 当前配置。
 */
static esp_err_t configure_station(const wifi_provisioning_profile_t *profile)
{
    wifi_config_t station_config = {0};
    strlcpy((char *)station_config.sta.ssid,
            profile->ssid,
            sizeof(station_config.sta.ssid));
    strlcpy((char *)station_config.sta.password,
            profile->password,
            sizeof(station_config.sta.password));
    station_config.sta.threshold.authmode = get_sta_auth_threshold();
    station_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    station_config.sta.sae_h2e_identifier[0] = '\0';
    return esp_wifi_set_config(WIFI_IF_STA, &station_config);
}

/**
 * @brief 将成功连接的配置提升到 NVS 队列首位。
 */
static void promote_connected_profile(const wifi_provisioning_profile_t *profile)
{
    wifi_provisioning_context_t *context = &g_wifi_provisioning_context;
    xSemaphoreTake(context->storage_mutex, portMAX_DELAY);
    esp_err_t error = wifi_provisioning_storage_save_profile(profile);
    xSemaphoreGive(context->storage_mutex);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "提升已连接 Wi-Fi 优先级失败：%s", esp_err_to_name(error));
    }
}

/**
 * @brief 尝试连接一条 Wi-Fi 配置，返回三次尝试的最终结果。
 */
static bool connect_profile_with_retries(const wifi_provisioning_profile_t *profile)
{
    wifi_provisioning_context_t *context = &g_wifi_provisioning_context;
    set_state(WIFI_PROVISIONING_STATE_CONNECTING);

    for (uint32_t retry = 0; retry < WIFI_PROVISIONING_CONNECT_RETRY_COUNT; ++retry) {
        xSemaphoreTake(context->state_mutex, portMAX_DELAY);
        context->connection_attempt_active = true;
        context->status.attempt_count++;
        xSemaphoreGive(context->state_mutex);

        xSemaphoreTake(context->wifi_operation_mutex, portMAX_DELAY);
        esp_err_t disconnect_error = esp_wifi_disconnect();
        if (disconnect_error == ESP_OK) {
            xEventGroupWaitBits(context->event_group,
                                WIFI_PROVISIONING_ATTEMPT_FAILED_BIT,
                                pdTRUE,
                                pdFALSE,
                                pdMS_TO_TICKS(500U));
        } else if (disconnect_error != ESP_ERR_WIFI_NOT_CONNECT) {
            ESP_LOGW(TAG,
                     "清理上一条 Wi-Fi 连接失败：%s",
                     esp_err_to_name(disconnect_error));
        }

        xEventGroupClearBits(context->event_group,
                             WIFI_PROVISIONING_ATTEMPT_SUCCESS_BIT |
                                 WIFI_PROVISIONING_ATTEMPT_FAILED_BIT);
        esp_err_t error = configure_station(profile);
        if (error == ESP_OK) {
            error = esp_wifi_connect();
        }
        xSemaphoreGive(context->wifi_operation_mutex);

        if (error != ESP_OK) {
            ESP_LOGW(TAG,
                     "连接 SSID=%s 第 %" PRIu32 " 次发起失败：%s",
                     profile->ssid,
                     retry + 1U,
                     esp_err_to_name(error));
            xSemaphoreTake(context->state_mutex, portMAX_DELAY);
            context->connection_attempt_active = false;
            xSemaphoreGive(context->state_mutex);
            continue;
        }

        EventBits_t bits = xEventGroupWaitBits(context->event_group,
                                               WIFI_PROVISIONING_ATTEMPT_SUCCESS_BIT |
                                                   WIFI_PROVISIONING_ATTEMPT_FAILED_BIT,
                                               pdTRUE,
                                               pdFALSE,
                                               pdMS_TO_TICKS(WIFI_PROVISIONING_CONNECT_TIMEOUT_MS));

        xSemaphoreTake(context->state_mutex, portMAX_DELAY);
        context->connection_attempt_active = false;
        xSemaphoreGive(context->state_mutex);

        if ((bits & WIFI_PROVISIONING_ATTEMPT_SUCCESS_BIT) != 0U) {
            xSemaphoreTake(context->state_mutex, portMAX_DELAY);
            strlcpy(context->status.current_ssid,
                    profile->ssid,
                    sizeof(context->status.current_ssid));
            xSemaphoreGive(context->state_mutex);
            promote_connected_profile(profile);
            ESP_LOGI(TAG, "已连接保存的 Wi-Fi：SSID=%s", profile->ssid);
            return true;
        }

        ESP_LOGW(TAG,
                 "连接 SSID=%s 第 %" PRIu32 " 次失败，继续尝试当前网络",
                 profile->ssid,
                 retry + 1U);
    }

    return false;
}

/**
 * @brief 按 NVS 优先级轮询全部保存的 Wi-Fi。
 */
static void connect_saved_profiles(void)
{
    wifi_provisioning_context_t *context = &g_wifi_provisioning_context;
    wifi_provisioning_profile_t profiles[WIFI_PROVISIONING_MAX_PROFILES] = {0};
    size_t profile_count = 0U;

    xSemaphoreTake(context->storage_mutex, portMAX_DELAY);
    esp_err_t error = wifi_provisioning_storage_get_profiles(profiles,
                                                              WIFI_PROVISIONING_MAX_PROFILES,
                                                              &profile_count);
    xSemaphoreGive(context->storage_mutex);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "读取保存的 Wi-Fi 失败：%s", esp_err_to_name(error));
        set_state(WIFI_PROVISIONING_STATE_PROVISIONING);
        return;
    }

    if (profile_count == 0U) {
        ESP_LOGI(TAG, "没有保存的 Wi-Fi，进入网页配网模式");
        set_state(WIFI_PROVISIONING_STATE_PROVISIONING);
        return;
    }

    set_state(WIFI_PROVISIONING_STATE_CONNECTING);
    for (size_t index = 0; index < profile_count; ++index) {
        if (connect_profile_with_retries(&profiles[index])) {
            return;
        }
    }

    ESP_LOGW(TAG, "已轮询 %u 条保存的 Wi-Fi 且每条均尝试三次，进入网页配网模式",
             (unsigned)profile_count);
    set_state(WIFI_PROVISIONING_STATE_PROVISIONING);
}

/**
 * @brief 连接后台任务，统一执行开机、断网和网页提交后的连接动作。
 */
static void connection_task(void *argument)
{
    (void)argument;
    wifi_provisioning_context_t *context = &g_wifi_provisioning_context;

    xEventGroupWaitBits(context->event_group,
                        WIFI_PROVISIONING_STA_STARTED_BIT,
                        pdFALSE,
                        pdTRUE,
                        portMAX_DELAY);
    connect_saved_profiles();

    while (true) {
        wifi_provisioning_command_t command;
        if (xQueueReceive(context->command_queue, &command, portMAX_DELAY) != pdPASS) {
            continue;
        }

        xSemaphoreTake(context->state_mutex, portMAX_DELAY);
        context->reconnect_command_pending = false;
        xSemaphoreGive(context->state_mutex);

        if (command.type == WIFI_PROVISIONING_COMMAND_CONNECT_PROFILE) {
            if (!connect_profile_with_retries(&command.profile)) {
                set_state(WIFI_PROVISIONING_STATE_PROVISIONING);
            }
        } else {
            connect_saved_profiles();
        }
    }
}

esp_err_t wifi_provisioning_enqueue_command(const wifi_provisioning_command_t *command)
{
    wifi_provisioning_context_t *context = &g_wifi_provisioning_context;
    if (command == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!context->started || context->command_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return xQueueSend(context->command_queue, command, 0) == pdPASS ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t wifi_provisioning_start(void)
{
    wifi_provisioning_context_t *context = &g_wifi_provisioning_context;
    if (context->started) {
        return ESP_ERR_INVALID_STATE;
    }

    memset(context, 0, sizeof(*context));
    context->status.state = WIFI_PROVISIONING_STATE_STARTING;
    context->state_mutex = xSemaphoreCreateMutex();
    context->wifi_operation_mutex = xSemaphoreCreateMutex();
    context->storage_mutex = xSemaphoreCreateMutex();
    context->event_group = xEventGroupCreate();
    context->command_queue = xQueueCreate(4U, sizeof(wifi_provisioning_command_t));
    if (context->state_mutex == NULL || context->wifi_operation_mutex == NULL ||
        context->storage_mutex == NULL || context->event_group == NULL ||
        context->command_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t error = wifi_provisioning_storage_init();
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi 配置 NVS 初始化失败：%s", esp_err_to_name(error));
        return error;
    }

    error = initialize_network_infrastructure();
    if (error != ESP_OK) {
        return error;
    }
    error = connect_c5_coprocessor();
    if (error != ESP_OK) {
        return error;
    }

    context->station_netif = esp_netif_create_default_wifi_sta();
    context->access_point_netif = esp_netif_create_default_wifi_ap();
    if (context->station_netif == NULL || context->access_point_netif == NULL) {
        return ESP_ERR_NO_MEM;
    }

    wifi_init_config_t wifi_init_config = WIFI_INIT_CONFIG_DEFAULT();
    error = esp_wifi_init(&wifi_init_config);
    if (error != ESP_OK) {
        return error;
    }

    error = esp_event_handler_instance_register(WIFI_EVENT,
                                                ESP_EVENT_ANY_ID,
                                                wifi_event_handler,
                                                NULL,
                                                &context->wifi_event_handler);
    if (error != ESP_OK) {
        return error;
    }
    error = esp_event_handler_instance_register(IP_EVENT,
                                                IP_EVENT_STA_GOT_IP,
                                                wifi_event_handler,
                                                NULL,
                                                &context->ip_event_handler);
    if (error != ESP_OK) {
        return error;
    }

    error = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (error != ESP_OK) {
        return error;
    }
    error = configure_access_point();
    if (error != ESP_OK) {
        return error;
    }

    context->started = true;
    error = esp_wifi_start();
    if (error != ESP_OK) {
        context->started = false;
        return error;
    }

    if (context->access_point_netif != NULL) {
        esp_netif_get_ip_info(context->access_point_netif, &context->status.ap_ip_info);
    }

    error = wifi_provisioning_web_start(&context->http_server);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "网页配网服务启动失败：%s", esp_err_to_name(error));
        return error;
    }

    BaseType_t task_result = xTaskCreate(connection_task,
                                         "wifi_connect",
                                         WIFI_PROVISIONING_TASK_STACK_SIZE,
                                         NULL,
                                         WIFI_PROVISIONING_TASK_PRIORITY,
                                         NULL);
    if (task_result != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG,
             "AP+STA 已启动，配网热点 SSID=%s，访问地址=http://" IPSTR,
             CONFIG_WIFI_PROVISIONING_AP_SSID,
             IP2STR(&context->status.ap_ip_info.ip));
    return ESP_OK;
}

esp_err_t wifi_provisioning_wait_connected(TickType_t timeout_ticks)
{
    wifi_provisioning_context_t *context = &g_wifi_provisioning_context;
    if (!context->started) {
        return ESP_ERR_INVALID_STATE;
    }

    EventBits_t bits = xEventGroupWaitBits(context->event_group,
                                           WIFI_PROVISIONING_CONNECTED_BIT,
                                           pdFALSE,
                                           pdTRUE,
                                           timeout_ticks);
    return (bits & WIFI_PROVISIONING_CONNECTED_BIT) != 0U ? ESP_OK : ESP_ERR_TIMEOUT;
}

bool wifi_provisioning_is_connected(void)
{
    wifi_provisioning_context_t *context = &g_wifi_provisioning_context;
    if (!context->started) {
        return false;
    }

    xSemaphoreTake(context->state_mutex, portMAX_DELAY);
    bool connected = context->status.sta_connected;
    xSemaphoreGive(context->state_mutex);
    return connected;
}

esp_err_t wifi_provisioning_get_status(wifi_provisioning_status_t *status)
{
    wifi_provisioning_context_t *context = &g_wifi_provisioning_context;
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!context->started) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(context->state_mutex, portMAX_DELAY);
    *status = context->status;
    xSemaphoreGive(context->state_mutex);
    return ESP_OK;
}

esp_err_t wifi_provisioning_get_profiles(wifi_provisioning_profile_t *profiles,
                                          size_t max_profiles,
                                          size_t *profile_count)
{
    wifi_provisioning_context_t *context = &g_wifi_provisioning_context;
    if (profiles == NULL || profile_count == NULL || max_profiles == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!context->started) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(context->storage_mutex, portMAX_DELAY);
    esp_err_t error = wifi_provisioning_storage_get_profiles(profiles,
                                                              max_profiles,
                                                              profile_count);
    xSemaphoreGive(context->storage_mutex);
    return error;
}

esp_err_t wifi_provisioning_save_profile(const wifi_provisioning_profile_t *profile)
{
    wifi_provisioning_context_t *context = &g_wifi_provisioning_context;
    if (profile == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!context->started) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(context->storage_mutex, portMAX_DELAY);
    esp_err_t error = wifi_provisioning_storage_save_profile(profile);
    xSemaphoreGive(context->storage_mutex);
    if (error != ESP_OK) {
        return error;
    }

    wifi_provisioning_command_t command = {
        .type = WIFI_PROVISIONING_COMMAND_CONNECT_PROFILE,
        .profile = *profile,
    };
    return wifi_provisioning_enqueue_command(&command);
}

esp_err_t wifi_provisioning_delete_profile(const char *ssid)
{
    wifi_provisioning_context_t *context = &g_wifi_provisioning_context;
    if (ssid == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!context->started) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(context->storage_mutex, portMAX_DELAY);
    esp_err_t error = wifi_provisioning_storage_delete_profile(ssid);
    xSemaphoreGive(context->storage_mutex);
    return error;
}

esp_err_t wifi_provisioning_request_reconnect(void)
{
    wifi_provisioning_command_t command = {
        .type = WIFI_PROVISIONING_COMMAND_RECONNECT_SAVED,
    };
    return wifi_provisioning_enqueue_command(&command);
}

esp_err_t wifi_provisioning_scan(wifi_ap_record_t *records,
                                 uint16_t max_records,
                                 uint16_t *record_count)
{
    wifi_provisioning_context_t *context = &g_wifi_provisioning_context;
    if (records == NULL || record_count == NULL || max_records == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!context->started) {
        return ESP_ERR_INVALID_STATE;
    }

    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 100,
        .scan_time.active.max = 300,
    };

    xSemaphoreTake(context->wifi_operation_mutex, portMAX_DELAY);
    esp_err_t error = esp_wifi_scan_start(&scan_config, true);
    if (error == ESP_OK) {
        uint16_t actual_count = 0U;
        error = esp_wifi_scan_get_ap_num(&actual_count);
        if (error == ESP_OK) {
            if (actual_count > max_records) {
                actual_count = max_records;
            }
            error = esp_wifi_scan_get_ap_records(&actual_count, records);
            if (error == ESP_OK) {
                *record_count = actual_count;
            }
        }
    }
    xSemaphoreGive(context->wifi_operation_mutex);
    return error;
}
