/**
 * @file pairing_ui.c
 * @brief 配对界面实现 - 本地HTTP服务器显示配对码
 */

#include "pairing_ui.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "pairing_ui";

// 全局状态
typedef enum {
    UI_STATE_IDLE,
    UI_STATE_WAITING,      // 等待配对码
    UI_STATE_SHOWING_CODE, // 显示配对码
    UI_STATE_SUCCESS,      // 配对成功
    UI_STATE_ERROR,        // 配对错误
} ui_state_t;

static httpd_handle_t s_server = NULL;
static char s_pairing_code[8] = {0};
static int s_expires_in = 0;
static ui_state_t s_state = UI_STATE_IDLE;
static char s_error_message[256] = {0};
static char s_device_ip[16] = {0};

// HTML页面模板
static const char *html_header =
    "<!DOCTYPE html>"
    "<html><head>"
    "<meta charset='UTF-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>设备配对</title>"
    "<style>"
    "body{font-family:Arial,sans-serif;text-align:center;padding:20px;background:#f0f0f0}"
    ".container{max-width:600px;margin:0 auto;background:white;padding:40px;border-radius:10px;box-shadow:0 2px 10px rgba(0,0,0,0.1)}"
    "h1{color:#333;margin-bottom:30px}"
    ".code{font-size:72px;font-weight:bold;color:#007bff;letter-spacing:10px;margin:30px 0;font-family:monospace}"
    ".info{color:#666;font-size:18px;margin:20px 0}"
    ".timer{color:#ff6b6b;font-size:24px;margin:20px 0}"
    ".success{color:#51cf66;font-size:24px}"
    ".error{color:#ff6b6b;font-size:18px}"
    ".loading{border:4px solid #f3f3f3;border-top:4px solid #007bff;border-radius:50%;width:40px;height:40px;animation:spin 1s linear infinite;margin:20px auto}"
    "@keyframes spin{0%{transform:rotate(0deg)}100%{transform:rotate(360deg)}}"
    ".ip{color:#999;font-size:14px;margin-top:30px}"
    "</style>"
    "</head><body><div class='container'>";

static const char *html_footer =
    "</div></body></html>";

// 前向声明
static esp_err_t root_handler(httpd_req_t *req);
static esp_err_t status_handler(httpd_req_t *req);
static void get_device_ip(void);

esp_err_t pairing_ui_start(void)
{
    if (s_server != NULL) {
        ESP_LOGW(TAG, "HTTP服务器已启动");
        return ESP_OK;
    }

    // 获取设备IP地址
    get_device_ip();

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.ctrl_port = 32768;
    config.max_open_sockets = 7;
    config.lru_purge_enable = true;

    ESP_LOGI(TAG, "启动HTTP服务器，端口: %d", config.server_port);

    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "启动HTTP服务器失败: %s", esp_err_to_name(err));
        return err;
    }

    // 注册URI处理器
    httpd_uri_t root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &root);

    httpd_uri_t status = {
        .uri = "/status",
        .method = HTTP_GET,
        .handler = status_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &status);

    s_state = UI_STATE_WAITING;
    ESP_LOGI(TAG, "配对网页已启动，访问地址: http://%s", s_device_ip);

    return ESP_OK;
}

esp_err_t pairing_ui_update_code(const char *code, int expires_in)
{
    if (code == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    strncpy(s_pairing_code, code, sizeof(s_pairing_code) - 1);
    s_pairing_code[sizeof(s_pairing_code) - 1] = '\0';
    s_expires_in = expires_in;
    s_state = UI_STATE_SHOWING_CODE;

    ESP_LOGI(TAG, "配对码已更新: %s (有效期: %d秒)", s_pairing_code, s_expires_in);
    return ESP_OK;
}

esp_err_t pairing_ui_show_success(void)
{
    s_state = UI_STATE_SUCCESS;
    ESP_LOGI(TAG, "显示配对成功页面");
    return ESP_OK;
}

esp_err_t pairing_ui_show_error(const char *error_message)
{
    if (error_message) {
        strncpy(s_error_message, error_message, sizeof(s_error_message) - 1);
        s_error_message[sizeof(s_error_message) - 1] = '\0';
    }
    s_state = UI_STATE_ERROR;
    ESP_LOGI(TAG, "显示配对错误页面: %s", s_error_message);
    return ESP_OK;
}

esp_err_t pairing_ui_stop(void)
{
    if (s_server == NULL) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "停止HTTP服务器");
    esp_err_t err = httpd_stop(s_server);
    s_server = NULL;
    s_state = UI_STATE_IDLE;

    return err;
}

bool pairing_ui_is_running(void)
{
    return (s_server != NULL);
}

// ========== 内部函数实现 ==========

static void get_device_ip(void)
{
    esp_netif_ip_info_t ip_info;
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");

    if (netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
        snprintf(s_device_ip, sizeof(s_device_ip), IPSTR, IP2STR(&ip_info.ip));
    } else {
        strcpy(s_device_ip, "0.0.0.0");
    }
}

static esp_err_t root_handler(httpd_req_t *req)
{
    char response[2048];
    int len = 0;

    // 添加HTML头部
    len += snprintf(response + len, sizeof(response) - len, "%s", html_header);

    // 根据状态显示不同内容
    switch (s_state) {
    case UI_STATE_WAITING:
        len += snprintf(response + len, sizeof(response) - len,
            "<h1>设备配对</h1>"
            "<div class='loading'></div>"
            "<p class='info'>正在连接配对服务器...</p>");
        break;

    case UI_STATE_SHOWING_CODE:
        len += snprintf(response + len, sizeof(response) - len,
            "<h1>设备配对</h1>"
            "<div class='code'>%s</div>"
            "<p class='info'>请在管理后台或小程序输入此配对码</p>"
            "<p class='timer'>有效期: %d 秒</p>"
            "<p class='info'>配对码有效期5分钟，请尽快输入</p>"
            "<script>"
            "let time=%d;"
            "setInterval(()=>{"
            "time--;"
            "if(time<=0){document.querySelector('.timer').textContent='配对码已过期，请刷新页面';return;}"
            "document.querySelector('.timer').textContent='有效期: '+time+' 秒';"
            "},1000);"
            "</script>",
            s_pairing_code, s_expires_in, s_expires_in);
        break;

    case UI_STATE_SUCCESS:
        len += snprintf(response + len, sizeof(response) - len,
            "<h1>✓ 配对成功</h1>"
            "<p class='success'>设备已成功配对！</p>"
            "<p class='info'>设备将在3秒后自动重启...</p>"
            "<script>setTimeout(()=>{window.location.reload();},3000);</script>");
        break;

    case UI_STATE_ERROR:
        len += snprintf(response + len, sizeof(response) - len,
            "<h1>✗ 配对失败</h1>"
            "<p class='error'>%s</p>"
            "<p class='info'><a href='/'>点击重试</a></p>",
            s_error_message[0] ? s_error_message : "未知错误");
        break;

    default:
        len += snprintf(response + len, sizeof(response) - len,
            "<h1>设备配对</h1>"
            "<p class='info'>准备中...</p>");
        break;
    }

    // 添加设备IP信息
    len += snprintf(response + len, sizeof(response) - len,
        "<p class='ip'>设备地址: %s</p>", s_device_ip);

    // 添加HTML尾部
    len += snprintf(response + len, sizeof(response) - len, "%s", html_footer);

    // 发送响应
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, response, len);

    return ESP_OK;
}

static esp_err_t status_handler(httpd_req_t *req)
{
    char response[256];

    snprintf(response, sizeof(response),
        "{\"state\":\"%s\",\"code\":\"%s\",\"expires_in\":%d}",
        s_state == UI_STATE_WAITING ? "waiting" :
        s_state == UI_STATE_SHOWING_CODE ? "showing" :
        s_state == UI_STATE_SUCCESS ? "success" : "error",
        s_pairing_code,
        s_expires_in);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, response, strlen(response));

    return ESP_OK;
}
