/**
 * @file pairing_ui.c
 * @brief 配对界面实现 - 通过WiFi配网HTTP服务器扩展显示配对码
 */

#include "pairing_ui.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "wifi_provisioning.h"
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

static char s_pairing_code[8] = {0};
static int s_expires_in = 0;
static ui_state_t s_state = UI_STATE_IDLE;
static char s_error_message[256] = {0};
static bool s_registered = false;
static bool s_pairing_mode = false;  // 是否处于配对模式

// 前向声明
static esp_err_t pairing_page_handler(httpd_req_t *req);
static esp_err_t pairing_status_handler(httpd_req_t *req);
static esp_err_t app_mode_handler(httpd_req_t *req);
static esp_err_t pairing_restart_handler(httpd_req_t *req);

// 注册到WiFi配网HTTP服务器的URI处理器
static const httpd_uri_t s_pairing_handlers[] = {
    {
        .uri = "/pairing",
        .method = HTTP_GET,
        .handler = pairing_page_handler,
        .user_ctx = NULL
    },
    {
        .uri = "/api/pairing-status",
        .method = HTTP_GET,
        .handler = pairing_status_handler,
        .user_ctx = NULL
    },
    {
        .uri = "/api/app-mode",
        .method = HTTP_GET,
        .handler = app_mode_handler,
        .user_ctx = NULL
    },
    {
        .uri = "/api/pairing-restart",
        .method = HTTP_POST,
        .handler = pairing_restart_handler,
        .user_ctx = NULL
    }
};

esp_err_t pairing_ui_start(void)
{
    if (s_registered) {
        ESP_LOGW(TAG, "配对页面已注册");
        return ESP_OK;
    }

    // 注册到WiFi配网HTTP服务器
    esp_err_t err = wifi_provisioning_register_web_extension(
        s_pairing_handlers,
        sizeof(s_pairing_handlers) / sizeof(s_pairing_handlers[0])
    );

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "注册配对页面失败: %s", esp_err_to_name(err));
        return err;
    }

    s_state = UI_STATE_WAITING;
    s_registered = true;
    s_pairing_mode = true;  // 设置为配对模式
    ESP_LOGI(TAG, "配对页面已注册到WiFi配网HTTP服务器");
    ESP_LOGI(TAG, "访问地址: http://<设备IP>/pairing");

    return ESP_OK;
}

void pairing_ui_set_mode(bool is_pairing_mode)
{
    s_pairing_mode = is_pairing_mode;
    ESP_LOGI(TAG, "配对模式: %s", is_pairing_mode ? "开启" : "关闭");
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
    // WiFi配网HTTP服务器会统一管理，这里不需要停止
    s_state = UI_STATE_IDLE;
    s_registered = false;
    ESP_LOGI(TAG, "配对UI已停止");
    return ESP_OK;
}

bool pairing_ui_is_running(void)
{
    return s_registered;
}

// ========== 内部函数实现 ==========

static esp_err_t pairing_page_handler(httpd_req_t *req)
{
    char response[3072];
    int len = 0;

    // HTML头部 - 简洁大屏设计
    len += snprintf(response + len, sizeof(response) - len,
        "<!DOCTYPE html>"
        "<html><head>"
        "<meta charset='UTF-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>设备配对</title>"
        "<style>"
        "*{margin:0;padding:0;box-sizing:border-box}"
        "body{font-family:Arial,'Microsoft YaHei',sans-serif;background:linear-gradient(135deg,#667eea 0%%,#764ba2 100%%);min-height:100vh;display:flex;align-items:center;justify-content:center;color:#fff}"
        ".container{text-align:center;padding:40px;max-width:800px;width:90%%}"
        ".back-link{position:fixed;top:20px;left:20px;color:#fff;text-decoration:none;font-size:16px;background:rgba(255,255,255,0.2);padding:10px 20px;border-radius:25px;transition:all 0.3s}"
        ".back-link:hover{background:rgba(255,255,255,0.3);transform:translateX(-5px)}"
        "h1{font-size:32px;margin-bottom:50px;font-weight:300;letter-spacing:2px}"
        ".code-box{background:rgba(255,255,255,0.15);backdrop-filter:blur(10px);border-radius:20px;padding:60px 40px;margin:30px 0;box-shadow:0 8px 32px rgba(0,0,0,0.1)}"
        ".code{font-size:96px;font-weight:bold;letter-spacing:20px;margin:20px 0;font-family:'Courier New',monospace;text-shadow:0 2px 10px rgba(0,0,0,0.2)}"
        ".timer{font-size:28px;margin:30px 0;opacity:0.9}"
        ".info{font-size:18px;line-height:1.8;opacity:0.85;margin:20px 0}"
        ".success-box,.error-box{padding:60px 40px}"
        ".success-icon,.error-icon{font-size:80px;margin-bottom:20px}"
        ".success-text{font-size:32px;margin:20px 0}"
        ".error-text{font-size:24px;margin:20px 0}"
        ".btn{display:inline-block;margin-top:30px;padding:15px 40px;background:rgba(255,255,255,0.3);border:2px solid #fff;border-radius:30px;color:#fff;text-decoration:none;font-size:18px;transition:all 0.3s;cursor:pointer}"
        ".btn:hover{background:rgba(255,255,255,0.5);transform:translateY(-2px)}"
        "</style>"
        "</head><body>"
        "<a href='/' class='back-link'>← 返回WiFi配网</a>"
        "<div class='container'>");

    // 根据状态显示内容
    switch (s_state) {
    case UI_STATE_WAITING:
        len += snprintf(response + len, sizeof(response) - len,
            "<h1>设备配对</h1>"
            "<div class='code-box'>"
            "<div class='info'>正在连接配对服务器...</div>"
            "</div>");
        break;

    case UI_STATE_SHOWING_CODE:
        len += snprintf(response + len, sizeof(response) - len,
            "<h1>设备配对</h1>"
            "<div class='code-box'>"
            "<div class='code' id='code'>%s</div>"
            "<div class='timer' id='timer'>有效期: %d 秒</div>"
            "<div class='info'>请在管理后台或小程序输入此配对码</div>"
            "<div class='info' style='font-size:14px;margin-top:30px'>配对码有效期5分钟</div>"
            "</div>"
            "<script>"
            "let time=%d;"
            "setInterval(()=>{"
            "time--;"
            "if(time<=0){document.getElementById('timer').textContent='配对码已过期';document.getElementById('timer').style.color='#ff6b6b';return;}"
            "document.getElementById('timer').textContent='有效期: '+time+' 秒';"
            "},1000);"
            "</script>",
            s_pairing_code, s_expires_in, s_expires_in);
        break;

    case UI_STATE_SUCCESS:
        len += snprintf(response + len, sizeof(response) - len,
            "<div class='success-box'>"
            "<div class='success-icon'>✓</div>"
            "<div class='success-text'>配对成功！</div>"
            "<div class='info'>设备将在3秒后自动重启</div>"
            "</div>"
            "<script>setTimeout(()=>{window.location.reload();},3000);</script>");
        break;

    case UI_STATE_ERROR:
        len += snprintf(response + len, sizeof(response) - len,
            "<div class='error-box'>"
            "<div class='error-icon'>✗</div>"
            "<div class='error-text'>配对失败</div>"
            "<div class='info'>%s</div>"
            "<a href='javascript:void(0)' class='btn' onclick='restartPairing()'>重新尝试</a>"
            "</div>"
            "<script>"
            "function restartPairing(){"
            "fetch('/api/pairing-restart',{method:'POST'})"
            ".then(r=>r.json())"
            ".then(d=>{if(d.success){document.getElementById('timer').textContent='正在重新连接...';setTimeout(()=>window.location.reload(),2000);}else{alert('重连失败: '+d.message);}})"
            ".catch(e=>{alert('请求失败: '+e);});"
            "}"
            "</script>",
            s_error_message[0] ? s_error_message : "未知错误");
        break;

    default:
        len += snprintf(response + len, sizeof(response) - len,
            "<h1>设备配对</h1>"
            "<div class='code-box'><div class='info'>准备中...</div></div>");
        break;
    }

    len += snprintf(response + len, sizeof(response) - len,
        "</div></body></html>");

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, response, len);

    return ESP_OK;
}

static esp_err_t pairing_status_handler(httpd_req_t *req)
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

static esp_err_t app_mode_handler(httpd_req_t *req)
{
    char response[64];

    snprintf(response, sizeof(response),
        "{\"pairing_mode\":%s}",
        s_pairing_mode ? "true" : "false");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, response, strlen(response));

    return ESP_OK;
}

static esp_err_t pairing_restart_handler(httpd_req_t *req)
{
    // 需要包含backend_pairing头文件才能调用restart函数
    extern esp_err_t backend_pairing_restart(void);

    char response[128];

    ESP_LOGI(TAG, "收到网页重连请求");

    // 调用后台配对重启函数
    esp_err_t err = backend_pairing_restart();

    if (err == ESP_OK) {
        snprintf(response, sizeof(response),
            "{\"success\":true,\"message\":\"配对重启成功\"}");
        s_state = UI_STATE_WAITING;  // 重置UI状态为等待
        ESP_LOGI(TAG, "配对重启成功");
    } else {
        snprintf(response, sizeof(response),
            "{\"success\":false,\"message\":\"配对重启失败: %s\"}",
            esp_err_to_name(err));
        ESP_LOGE(TAG, "配对重启失败: %s", esp_err_to_name(err));
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, response, strlen(response));

    return ESP_OK;
}
