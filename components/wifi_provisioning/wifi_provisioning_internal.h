#pragma once

/**
 * @file wifi_provisioning_internal.h
 * @brief Wi-Fi 网页配网组件内部共享的数据结构和函数声明。
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "wifi_provisioning.h"

/** 组件最多保存八条 Wi-Fi，避免无限占用 NVS 和连接轮询时间。 */
#define WIFI_PROVISIONING_MAX_PROFILES 8U

/** HTTP 扫描接口单次最多返回二十个接入点，满足手机配网页面的常规使用。 */
#define WIFI_PROVISIONING_MAX_SCAN_RESULTS 20U

/** STA 获取 DHCP 地址后置位，业务层等待联网时使用该事件位。 */
#define WIFI_PROVISIONING_CONNECTED_BIT BIT0

/** 单次连接尝试获取 DHCP 地址后置位，连接管理任务据此结束等待。 */
#define WIFI_PROVISIONING_ATTEMPT_SUCCESS_BIT BIT1

/** 单次连接尝试发生断开后置位，连接管理任务据此切换下一个网络。 */
#define WIFI_PROVISIONING_ATTEMPT_FAILED_BIT BIT2

/** Wi-Fi 驱动完成 STA 启动后置位，确保管理任务不会过早调用连接接口。 */
#define WIFI_PROVISIONING_STA_STARTED_BIT BIT3

/**
 * @brief 后台连接任务接收的命令类型。
 */
typedef enum {
    WIFI_PROVISIONING_COMMAND_RECONNECT_SAVED = 0,
    WIFI_PROVISIONING_COMMAND_CONNECT_PROFILE,
} wifi_provisioning_command_type_t;

/**
 * @brief 后台连接任务命令。
 */
typedef struct {
    wifi_provisioning_command_type_t type;
    wifi_provisioning_profile_t profile;
} wifi_provisioning_command_t;

/**
 * @brief 组件唯一运行上下文。
 */
typedef struct {
    /** 保护状态快照和连接控制标志的互斥锁。 */
    SemaphoreHandle_t state_mutex;
    /** 串行化扫描、设置配置和连接操作的互斥锁。 */
    SemaphoreHandle_t wifi_operation_mutex;
    /** 保存、删除和读取 NVS 配置时使用的互斥锁。 */
    SemaphoreHandle_t storage_mutex;
    /** Wi-Fi 事件组。 */
    EventGroupHandle_t event_group;
    /** 后台连接命令队列。 */
    QueueHandle_t command_queue;
    /** 默认 STA 网络接口。 */
    esp_netif_t *station_netif;
    /** 默认 SoftAP 网络接口。 */
    esp_netif_t *access_point_netif;
    /** HTTP 服务句柄。 */
    httpd_handle_t http_server;
    /** Wi-Fi 事件处理器实例。 */
    esp_event_handler_instance_t wifi_event_handler;
    /** DHCP 地址事件处理器实例。 */
    esp_event_handler_instance_t ip_event_handler;
    /** 供业务层和网页读取的当前状态。 */
    wifi_provisioning_status_t status;
    /** 当前是否处于由管理任务发起的连接尝试中。 */
    bool connection_attempt_active;
    /** 断网重连命令是否已经入队，防止同一断网事件重复排队。 */
    bool reconnect_command_pending;
    /** 组件是否已完整启动。 */
    bool started;
} wifi_provisioning_context_t;

/** 全组件共享的唯一上下文，在 wifi_provisioning.c 中定义。 */
extern wifi_provisioning_context_t g_wifi_provisioning_context;

/**
 * @brief 初始化 NVS 配置存储。
 */
esp_err_t wifi_provisioning_storage_init(void);

/**
 * @brief 按最近成功或最近保存的优先级读取配置。
 */
esp_err_t wifi_provisioning_storage_get_profiles(wifi_provisioning_profile_t *profiles,
                                                  size_t max_profiles,
                                                  size_t *profile_count);

/**
 * @brief 保存或更新配置并提升到最高优先级。
 */
esp_err_t wifi_provisioning_storage_save_profile(const wifi_provisioning_profile_t *profile);

/**
 * @brief 删除指定 SSID 的配置。
 */
esp_err_t wifi_provisioning_storage_delete_profile(const char *ssid);

/**
 * @brief 启动网页服务并注册全部配网接口。
 */
esp_err_t wifi_provisioning_web_start(httpd_handle_t *server);

/**
 * @brief 以线程安全方式扫描附近 Wi-Fi。
 */
esp_err_t wifi_provisioning_scan(wifi_ap_record_t *records,
                                 uint16_t max_records,
                                 uint16_t *record_count);

/**
 * @brief 将连接命令写入后台队列。
 */
esp_err_t wifi_provisioning_enqueue_command(const wifi_provisioning_command_t *command);

