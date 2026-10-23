#include "p4_c5_wifi.h"

#include <inttypes.h>
#include <string.h>

#include "esp_event.h"
#include "esp_hosted.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/event_groups.h"
#include "freertos/timers.h"
#include "lwip/ip_addr.h"
#include "ping/ping_sock.h"

/* 网络事件位：CONNECTED 表示已经取得 DHCP 地址，INITIAL_FAILED 表示首次重试次数已耗尽。 */
#define P4_C5_WIFI_CONNECTED_BIT      BIT0
#define P4_C5_WIFI_INITIAL_FAILED_BIT BIT1

/* Ping 事件位用于等待异步 ICMP 会话结束，避免在会话完成前释放句柄。 */
#define P4_C5_WIFI_PING_DONE_BIT BIT0

/* 重连基础间隔固定为一秒，连续失败时按次数线性退避并受配置上限约束。 */
#define P4_C5_WIFI_RECONNECT_BASE_DELAY_MS 1000U

static const char *TAG = "p4_c5_wifi";

static EventGroupHandle_t s_wifi_event_group;
static EventGroupHandle_t s_ping_event_group;
static TimerHandle_t s_reconnect_timer;
static esp_netif_t *s_station_netif;
static esp_netif_ip_info_t s_ip_info;
static esp_event_handler_instance_t s_wifi_event_handler;
static esp_event_handler_instance_t s_ip_event_handler;
static uint32_t s_reconnect_count;
static bool s_started;

/* 将 menuconfig 选择的最低认证方式转换为 ESP-IDF Wi-Fi 驱动枚举。 */
#if CONFIG_P4_C5_WIFI_AUTH_OPEN
#define P4_C5_WIFI_AUTH_THRESHOLD WIFI_AUTH_OPEN
#elif CONFIG_P4_C5_WIFI_AUTH_WPA2_PSK
#define P4_C5_WIFI_AUTH_THRESHOLD WIFI_AUTH_WPA2_PSK
#elif CONFIG_P4_C5_WIFI_AUTH_WPA2_WPA3_PSK
#define P4_C5_WIFI_AUTH_THRESHOLD WIFI_AUTH_WPA2_WPA3_PSK
#endif

/**
 * @brief 计算当前连续断线次数对应的重连等待时间。
 *
 * 等待时间按照一秒、两秒、三秒递增，达到 menuconfig 配置的最大等待时间后
 * 保持该上限，避免网络长时间不可用时高频占用事件循环和无线协处理器。
 */
static uint32_t calculate_reconnect_delay_ms(void)
{
    uint64_t delay_ms = (uint64_t)s_reconnect_count * P4_C5_WIFI_RECONNECT_BASE_DELAY_MS;
    if (delay_ms == 0) {
        delay_ms = P4_C5_WIFI_RECONNECT_BASE_DELAY_MS;
    }
    if (delay_ms > CONFIG_P4_C5_WIFI_RECONNECT_MAX_DELAY_MS) {
        delay_ms = CONFIG_P4_C5_WIFI_RECONNECT_MAX_DELAY_MS;
    }
    return (uint32_t)delay_ms;
}

/**
 * @brief FreeRTOS 软件定时器回调，在事件循环之外发起下一次关联。
 *
 * Wi-Fi 断线事件只负责安排定时器，不在系统事件任务中阻塞等待，从而保证
 * IP、Wi-Fi 以及后续音视频组件的事件仍能及时处理。
 */
static void reconnect_timer_callback(TimerHandle_t timer)
{
    (void)timer;
    esp_err_t error = esp_wifi_connect();
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "发起 Wi-Fi 重连失败：%s", esp_err_to_name(error));
    }
}

/**
 * @brief 根据连续失败次数启动或重新安排一次重连。
 */
static void schedule_reconnect(void)
{
    uint32_t delay_ms = calculate_reconnect_delay_ms();
    TickType_t delay_ticks = pdMS_TO_TICKS(delay_ms);
    if (delay_ticks == 0) {
        delay_ticks = 1;
    }

    if (xTimerChangePeriod(s_reconnect_timer, delay_ticks, 0) != pdPASS) {
        ESP_LOGE(TAG, "设置 Wi-Fi 重连定时器失败");
        return;
    }
    if (xTimerStart(s_reconnect_timer, 0) != pdPASS) {
        ESP_LOGE(TAG, "启动 Wi-Fi 重连定时器失败");
        return;
    }

    ESP_LOGW(TAG, "将在 %" PRIu32 " 毫秒后执行第 %" PRIu32 " 次重连",
             delay_ms, s_reconnect_count);
}

/**
 * @brief 统一处理 Wi-Fi 与 DHCP 事件，并维护供上层查询的连接状态。
 */
static void network_event_handler(void *handler_arg,
                                  esp_event_base_t event_base,
                                  int32_t event_id,
                                  void *event_data)
{
    (void)handler_arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        /* Station 启动后立即进行第一次关联，不经过退避定时器。 */
        esp_err_t error = esp_wifi_connect();
        if (error != ESP_OK) {
            ESP_LOGE(TAG, "首次发起 Wi-Fi 连接失败：%s", esp_err_to_name(error));
        }
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *event =
            (const wifi_event_sta_disconnected_t *)event_data;

        memset(&s_ip_info, 0, sizeof(s_ip_info));
        xEventGroupClearBits(s_wifi_event_group, P4_C5_WIFI_CONNECTED_BIT);
        s_reconnect_count++;

        ESP_LOGW(TAG, "Wi-Fi 已断开，原因码=%u", event != NULL ? event->reason : 0U);

        /* 首次启动阶段达到指定次数时解除调用者等待，但后台重连不会停止。 */
        if (s_reconnect_count >= CONFIG_P4_C5_WIFI_INITIAL_RETRY_COUNT) {
            xEventGroupSetBits(s_wifi_event_group, P4_C5_WIFI_INITIAL_FAILED_BIT);
        }

        schedule_reconnect();
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)event_data;
        if (event == NULL) {
            ESP_LOGE(TAG, "收到空的 DHCP 地址事件");
            return;
        }

        s_ip_info = event->ip_info;
        s_reconnect_count = 0;
        xTimerStop(s_reconnect_timer, 0);
        xEventGroupClearBits(s_wifi_event_group, P4_C5_WIFI_INITIAL_FAILED_BIT);
        xEventGroupSetBits(s_wifi_event_group, P4_C5_WIFI_CONNECTED_BIT);

        ESP_LOGI(TAG, "P4 已通过 C5 联网，IP=" IPSTR "，网关=" IPSTR,
                 IP2STR(&s_ip_info.ip), IP2STR(&s_ip_info.gw));
    }
}

/**
 * @brief Ping 会话结束回调，打印网关收发统计并通知等待任务释放会话。
 */
static void gateway_ping_end_callback(esp_ping_handle_t ping_handle, void *callback_arg)
{
    (void)callback_arg;
    uint32_t transmitted = 0;
    uint32_t received = 0;
    uint32_t duration_ms = 0;

    esp_ping_get_profile(ping_handle, ESP_PING_PROF_REQUEST, &transmitted, sizeof(transmitted));
    esp_ping_get_profile(ping_handle, ESP_PING_PROF_REPLY, &received, sizeof(received));
    esp_ping_get_profile(ping_handle, ESP_PING_PROF_DURATION, &duration_ms, sizeof(duration_ms));

    ESP_LOGI(TAG,
             "C5 数据通道网关测试完成：发送=%" PRIu32 "，接收=%" PRIu32 "，耗时=%" PRIu32 " 毫秒",
             transmitted, received, duration_ms);
    xEventGroupSetBits(s_ping_event_group, P4_C5_WIFI_PING_DONE_BIT);
}

/**
 * @brief 对 DHCP 网关执行有限次数 Ping，验证 P4 到 C5 再到接入点的数据路径。
 */
static esp_err_t ping_gateway(void)
{
    if (CONFIG_P4_C5_WIFI_GATEWAY_PING_COUNT == 0) {
        return ESP_OK;
    }

    s_ping_event_group = xEventGroupCreate();
    if (s_ping_event_group == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_ping_config_t ping_config = ESP_PING_DEFAULT_CONFIG();
    ping_config.count = CONFIG_P4_C5_WIFI_GATEWAY_PING_COUNT;
    ping_config.interval_ms = 200;
    ping_config.timeout_ms = 1000;
    ip_addr_set_ip4_u32(&ping_config.target_addr, s_ip_info.gw.addr);

    esp_ping_callbacks_t callbacks = {
        .cb_args = NULL,
        .on_ping_success = NULL,
        .on_ping_timeout = NULL,
        .on_ping_end = gateway_ping_end_callback,
    };

    esp_ping_handle_t ping_handle = NULL;
    esp_err_t error = esp_ping_new_session(&ping_config, &callbacks, &ping_handle);
    if (error == ESP_OK) {
        ESP_LOGI(TAG, "开始验证网关 " IPSTR, IP2STR(&s_ip_info.gw));
        error = esp_ping_start(ping_handle);
    }

    if (error == ESP_OK) {
        xEventGroupWaitBits(s_ping_event_group,
                            P4_C5_WIFI_PING_DONE_BIT,
                            pdTRUE,
                            pdTRUE,
                            portMAX_DELAY);
    }

    if (ping_handle != NULL) {
        esp_err_t delete_error = esp_ping_delete_session(ping_handle);
        if (error == ESP_OK) {
            error = delete_error;
        }
    }

    vEventGroupDelete(s_ping_event_group);
    s_ping_event_group = NULL;
    return error;
}

/**
 * @brief 初始化 ESP-IDF 网络基础设施，兼容未来与音频、摄像头模块共享事件循环。
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
 * @brief 显式建立 ESP32-P4 与 ESP32-C5 之间的 ESP-Hosted 传输链路。
 *
 * esp_hosted_init() 在组件构造阶段已执行一次，并且官方实现允许重复调用。
 * 此处再次调用用于形成明确的应用初始化边界，再通过 connect 接口验证 SDIO
 * 从机已经响应，只有链路成功后才继续调用远程 Wi-Fi API。
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

    ESP_LOGI(TAG, "ESP32-P4 与 ESP32-C5 的 ESP-Hosted 链路已建立");
    return ESP_OK;
}

esp_err_t p4_c5_wifi_start(void)
{
    if (s_started) {
        return ESP_ERR_INVALID_STATE;
    }

    s_wifi_event_group = xEventGroupCreate();
    if (s_wifi_event_group == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_reconnect_timer = xTimerCreate("p4_c5_wifi_reconnect",
                                     pdMS_TO_TICKS(P4_C5_WIFI_RECONNECT_BASE_DELAY_MS),
                                     pdFALSE,
                                     NULL,
                                     reconnect_timer_callback);
    if (s_reconnect_timer == NULL) {
        vEventGroupDelete(s_wifi_event_group);
        s_wifi_event_group = NULL;
        return ESP_ERR_NO_MEM;
    }

    esp_err_t error = initialize_network_infrastructure();
    if (error != ESP_OK) {
        return error;
    }

    error = connect_c5_coprocessor();
    if (error != ESP_OK) {
        return error;
    }

    s_station_netif = esp_netif_create_default_wifi_sta();
    if (s_station_netif == NULL) {
        return ESP_FAIL;
    }

    wifi_init_config_t wifi_init_config = WIFI_INIT_CONFIG_DEFAULT();
    error = esp_wifi_init(&wifi_init_config);
    if (error != ESP_OK) {
        return error;
    }

    error = esp_event_handler_instance_register(WIFI_EVENT,
                                                ESP_EVENT_ANY_ID,
                                                network_event_handler,
                                                NULL,
                                                &s_wifi_event_handler);
    if (error != ESP_OK) {
        return error;
    }

    error = esp_event_handler_instance_register(IP_EVENT,
                                                IP_EVENT_STA_GOT_IP,
                                                network_event_handler,
                                                NULL,
                                                &s_ip_event_handler);
    if (error != ESP_OK) {
        return error;
    }

    wifi_config_t station_config = {0};
    strlcpy((char *)station_config.sta.ssid,
            CONFIG_P4_C5_WIFI_SSID,
            sizeof(station_config.sta.ssid));
    strlcpy((char *)station_config.sta.password,
            CONFIG_P4_C5_WIFI_PASSWORD,
            sizeof(station_config.sta.password));
    station_config.sta.threshold.authmode = P4_C5_WIFI_AUTH_THRESHOLD;
    station_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    station_config.sta.sae_h2e_identifier[0] = '\0';

    error = esp_wifi_set_mode(WIFI_MODE_STA);
    if (error != ESP_OK) {
        return error;
    }
    error = esp_wifi_set_config(WIFI_IF_STA, &station_config);
    if (error != ESP_OK) {
        return error;
    }

    s_started = true;
    error = esp_wifi_start();
    if (error != ESP_OK) {
        s_started = false;
        return error;
    }

    ESP_LOGI(TAG, "正在连接 Wi-Fi：SSID=%s", CONFIG_P4_C5_WIFI_SSID);
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           P4_C5_WIFI_CONNECTED_BIT |
                                               P4_C5_WIFI_INITIAL_FAILED_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           portMAX_DELAY);

    if ((bits & P4_C5_WIFI_CONNECTED_BIT) != 0) {
        esp_err_t ping_error = ping_gateway();
        if (ping_error != ESP_OK) {
            ESP_LOGW(TAG, "网关 Ping 会话执行失败：%s", esp_err_to_name(ping_error));
        }
        return ESP_OK;
    }

    ESP_LOGE(TAG,
             "首次连接已达到 %d 次重试上限，后台自动重连继续运行",
             CONFIG_P4_C5_WIFI_INITIAL_RETRY_COUNT);
    return ESP_FAIL;
}

esp_err_t p4_c5_wifi_wait_connected(TickType_t timeout_ticks)
{
    if (!s_started || s_wifi_event_group == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           P4_C5_WIFI_CONNECTED_BIT,
                                           pdFALSE,
                                           pdTRUE,
                                           timeout_ticks);
    return (bits & P4_C5_WIFI_CONNECTED_BIT) != 0 ? ESP_OK : ESP_ERR_TIMEOUT;
}

bool p4_c5_wifi_is_connected(void)
{
    if (!s_started || s_wifi_event_group == NULL) {
        return false;
    }
    return (xEventGroupGetBits(s_wifi_event_group) & P4_C5_WIFI_CONNECTED_BIT) != 0;
}

esp_err_t p4_c5_wifi_get_ip_info(esp_netif_ip_info_t *ip_info)
{
    if (ip_info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!p4_c5_wifi_is_connected()) {
        return ESP_ERR_INVALID_STATE;
    }

    *ip_info = s_ip_info;
    return ESP_OK;
}
