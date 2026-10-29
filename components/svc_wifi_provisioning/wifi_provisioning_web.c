#include "wifi_provisioning_internal.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "lwip/ip4_addr.h"

/** POST 表单最大长度，足以容纳 32 字节 SSID、64 字节密码及 URL 编码扩展。 */
#define WIFI_PROVISIONING_FORM_BUFFER_SIZE 384U

/** JSON 字符串临时缓冲区，单个 SSID 转义后不会超过该容量。 */
#define WIFI_PROVISIONING_JSON_STRING_SIZE 192U

static const char *TAG = "wifi_web";
static httpd_uri_t s_extensions[12];
static size_t s_extension_count;
static bool s_web_started;

esp_err_t wifi_provisioning_register_web_extension(const httpd_uri_t *handlers, size_t count)
{
    if (!handlers || !count || count > 12 - s_extension_count) return ESP_ERR_INVALID_ARG;
    if (s_web_started) return ESP_ERR_INVALID_STATE;
    for (size_t i = 0; i < count; ++i) {
        if (!handlers[i].uri || !handlers[i].handler) return ESP_ERR_INVALID_ARG;
        for (size_t j = 0; j < s_extension_count + i; ++j) {
            const httpd_uri_t *other = j < s_extension_count ? &s_extensions[j] : &handlers[j - s_extension_count];
            if (other->method == handlers[i].method && !strcmp(other->uri, handlers[i].uri))
                return ESP_ERR_INVALID_ARG;
        }
    }
    memcpy(s_extensions + s_extension_count, handlers, count * sizeof(*handlers));
    s_extension_count += count;
    return ESP_OK;
}

/**
 * @brief 内置的单页配网界面。
 *
 * 页面不依赖互联网、CDN 或外部文件，手机连接设备热点后访问 192.168.4.1
 * 即可完成扫描、连接、查看当前网络和管理已保存网络。
 */
static const char s_index_html[] =
    "<!doctype html><html lang='zh-CN'><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>设备 Wi-Fi 配网</title><style>"
    "*{box-sizing:border-box}body{margin:0;background:#f3f5f8;color:#1f2937;font-family:Arial,'Microsoft YaHei',sans-serif}"
    ".wrap{max-width:720px;margin:auto;padding:18px}.card{background:#fff;border-radius:14px;padding:18px;margin-bottom:14px;box-shadow:0 4px 18px #00000012}"
    "h1{font-size:24px;margin:4px 0 16px}h2{font-size:17px;margin:0 0 12px}.status{display:grid;grid-template-columns:110px 1fr;gap:8px;font-size:14px}"
    "button,input{font-size:16px;border-radius:9px;border:1px solid #d1d5db;padding:11px}button{border:0;background:#2563eb;color:#fff;cursor:pointer}"
    "button.secondary{background:#64748b}button.danger{background:#dc2626;padding:7px 10px;font-size:13px}input{width:100%;margin-bottom:10px}"
    ".row{display:flex;gap:9px}.row button{flex:1}.network{display:flex;align-items:center;justify-content:space-between;border-top:1px solid #eef0f3;padding:11px 0}"
    ".network:first-child{border-top:0}.network-main{min-width:0}.ssid{font-weight:600;overflow:hidden;text-overflow:ellipsis}.meta{font-size:12px;color:#64748b;margin-top:3px}"
    ".tip{font-size:13px;color:#64748b;line-height:1.6}.message{min-height:22px;color:#2563eb;font-size:14px;margin-top:9px}"
    "#pairingBanner{display:none;background:linear-gradient(135deg,#667eea,#764ba2);color:#fff;padding:18px;text-align:center;border-radius:14px;margin-bottom:14px;box-shadow:0 4px 18px #00000012}"
    "#pairingBanner a{color:#fff;text-decoration:none;font-size:18px;font-weight:600;display:block}"
    "#pairingBanner a:hover{opacity:0.9}</style></head><body>"
    "<div class='wrap'>"
    "<div id='pairingBanner'><a href='/pairing'>📱 设备配对 - 点击查看配对码 →</a></div>"
    "<h1>设备 Wi-Fi 配网</h1>"
    "<div class='card'><h2>当前状态</h2><div class='status'>"
    "<span>运行状态</span><b id='state'>读取中</b><span>当前 Wi-Fi</span><b id='current'>-</b>"
    "<span>STA 地址</span><b id='staIp'>-</b><span>配网地址</span><b id='apIp'>192.168.4.1</b>"
    "<span>连接尝试</span><b id='attempts'>0</b></div></div>"
    "<div class='card'><h2>连接 Wi-Fi</h2><input id='ssid' maxlength='32' placeholder='Wi-Fi 名称'>"
    "<input id='password' type='password' maxlength='64' placeholder='Wi-Fi 密码'>"
    "<div class='row'><button onclick='connectWifi()'>保存并连接</button><button class='secondary' onclick='scanWifi()'>扫描附近 Wi-Fi</button></div>"
    "<div id='message' class='message'></div><div id='scanList'></div></div>"
    "<div class='card'><h2>已保存 Wi-Fi</h2><div id='savedList' class='tip'>读取中</div></div>"
    "<div class='tip'>设备始终保持 AP+STA 模式。开机或断网后会按列表顺序轮询，每个 Wi-Fi 最多尝试 3 次；全部失败后保持热点和本页面继续配网。</div>"
    "</div><script>"
    "const stateText={0:'已停止',1:'启动中',2:'正在连接',3:'已联网',4:'等待配网'};"
    "async function getJson(url,opt){const r=await fetch(url,opt);const j=await r.json();if(!r.ok)throw new Error(j.message||'请求失败');return j;}"
    "async function refresh(){try{const s=await getJson('/api/status');state.textContent=stateText[s.state]||'未知';current.textContent=s.current_ssid||'-';staIp.textContent=s.sta_ip||'-';apIp.textContent=s.ap_ip||'192.168.4.1';attempts.textContent=s.attempt_count;}catch(e){message.textContent=e.message;}try{const d=await getJson('/api/saved');savedList.innerHTML=d.networks.length?d.networks.map(n=>`<div class='network'><div class='network-main'><div class='ssid'>${escapeHtml(n.ssid)}</div><div class='meta'>已保存，密码不会在网页显示</div></div><button class='danger' onclick=\"removeWifi(decodeURIComponent('${attr(n.ssid)}'))\">删除</button></div>`).join(''):'暂无已保存 Wi-Fi';}catch(e){savedList.textContent=e.message;}}"
    "async function scanWifi(){message.textContent='正在扫描，请稍候…';scanList.innerHTML='';try{const d=await getJson('/api/scan');scanList.innerHTML=d.networks.map(n=>`<div class='network'><div class='network-main'><div class='ssid'>${escapeHtml(n.ssid)||'(隐藏网络)'}</div><div class='meta'>信号 ${n.rssi} dBm · ${n.auth}</div></div><button onclick=\"selectWifi(decodeURIComponent('${attr(n.ssid)}'))\">选择</button></div>`).join('')||`<div class='tip'>没有扫描到 Wi-Fi</div>`;message.textContent=`扫描完成，共 ${d.networks.length} 个网络`;}catch(e){message.textContent=e.message;}}"
    "function selectWifi(name){ssid.value=name;password.focus();}"
    "async function connectWifi(){const name=ssid.value.trim();if(!name){message.textContent='请输入 Wi-Fi 名称';return;}message.textContent='配置已提交，设备将最多尝试 3 次连接';const body=new URLSearchParams({ssid:name,password:password.value});try{await getJson('/api/connect',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body});password.value='';await refresh();}catch(e){message.textContent=e.message;}}"
    "async function removeWifi(name){if(!confirm(`确认删除 ${name}？`))return;const body=new URLSearchParams({ssid:name});try{await getJson('/api/delete',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body});await refresh();}catch(e){message.textContent=e.message;}}"
    "function attr(v){return encodeURIComponent(v).replace(/'/g,'%27');}"
    "function escapeHtml(v){return String(v).replace(/[&<>\"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','\"':'&quot;',\"'\":'&#39;'}[c]));}"
    "fetch('/api/app-mode').then(r=>r.json()).then(d=>{if(d.pairing_mode)document.getElementById('pairingBanner').style.display='block';}).catch(()=>{});"
    "refresh();setInterval(refresh,3000);</script></body></html>";

/**
 * @brief 将运行状态转换为网页可读的固定英文标识。
 */
static const char *auth_mode_to_text(wifi_auth_mode_t auth_mode)
{
    switch (auth_mode) {
        case WIFI_AUTH_OPEN:
            return "OPEN";
        case WIFI_AUTH_WEP:
            return "WEP";
        case WIFI_AUTH_WPA_PSK:
            return "WPA";
        case WIFI_AUTH_WPA2_PSK:
            return "WPA2";
        case WIFI_AUTH_WPA_WPA2_PSK:
            return "WPA/WPA2";
        case WIFI_AUTH_WPA3_PSK:
            return "WPA3";
        case WIFI_AUTH_WPA2_WPA3_PSK:
            return "WPA2/WPA3";
        default:
            return "OTHER";
    }
}

/**
 * @brief 对 JSON 字符串中的控制字符、引号和反斜杠进行转义。
 */
static void json_escape(const char *source, char *destination, size_t destination_size)
{
    size_t write_index = 0U;
    for (size_t read_index = 0U; source[read_index] != '\0' && write_index + 1U < destination_size;
         ++read_index) {
        unsigned char character = (unsigned char)source[read_index];
        if ((character == '"' || character == '\\') && write_index + 2U < destination_size) {
            destination[write_index++] = '\\';
            destination[write_index++] = (char)character;
        } else if (character >= 0x20U) {
            destination[write_index++] = (char)character;
        }
    }
    destination[write_index] = '\0';
}

/**
 * @brief 将十六进制字符转换为数值，供 URL 解码使用。
 */
static int hexadecimal_value(char character)
{
    if (character >= '0' && character <= '9') {
        return character - '0';
    }
    character = (char)tolower((unsigned char)character);
    if (character >= 'a' && character <= 'f') {
        return character - 'a' + 10;
    }
    return -1;
}

/**
 * @brief 原地解码 application/x-www-form-urlencoded 字段。
 */
static void url_decode(char *value)
{
    char *read_pointer = value;
    char *write_pointer = value;
    while (*read_pointer != '\0') {
        if (*read_pointer == '+') {
            *write_pointer++ = ' ';
            ++read_pointer;
        } else if (*read_pointer == '%' && read_pointer[1] != '\0' && read_pointer[2] != '\0') {
            int high = hexadecimal_value(read_pointer[1]);
            int low = hexadecimal_value(read_pointer[2]);
            if (high >= 0 && low >= 0) {
                *write_pointer++ = (char)((high << 4) | low);
                read_pointer += 3;
            } else {
                *write_pointer++ = *read_pointer++;
            }
        } else {
            *write_pointer++ = *read_pointer++;
        }
    }
    *write_pointer = '\0';
}

/**
 * @brief 读取完整 POST 请求体，拒绝超出固定缓冲区的请求。
 */
static esp_err_t receive_request_body(httpd_req_t *request, char *buffer, size_t buffer_size)
{
    if ((size_t)request->content_len >= buffer_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    size_t received_total = 0U;
    while (received_total < (size_t)request->content_len) {
        int received = httpd_req_recv(request,
                                      buffer + received_total,
                                      request->content_len - received_total);
        if (received == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (received <= 0) {
            return ESP_FAIL;
        }
        received_total += (size_t)received;
    }
    buffer[received_total] = '\0';
    return ESP_OK;
}

/**
 * @brief 从 URL 编码表单中提取指定字段并完成解码。
 */
static esp_err_t get_form_value(char *form,
                                const char *key,
                                char *value,
                                size_t value_size,
                                bool required)
{
    esp_err_t error = httpd_query_key_value(form, key, value, value_size);
    if (error == ESP_ERR_NOT_FOUND && !required) {
        value[0] = '\0';
        return ESP_OK;
    }
    if (error != ESP_OK) {
        return error;
    }
    url_decode(value);
    return ESP_OK;
}

/**
 * @brief 发送统一格式的 JSON 结果。
 */
static esp_err_t send_result(httpd_req_t *request,
                             const char *http_status,
                             bool success,
                             const char *message)
{
    char escaped_message[WIFI_PROVISIONING_JSON_STRING_SIZE];
    char response[WIFI_PROVISIONING_JSON_STRING_SIZE + 48U];
    json_escape(message, escaped_message, sizeof(escaped_message));
    snprintf(response,
             sizeof(response),
             "{\"success\":%s,\"message\":\"%s\"}",
             success ? "true" : "false",
             escaped_message);
    httpd_resp_set_status(request, http_status);
    httpd_resp_set_type(request, "application/json; charset=utf-8");
    return httpd_resp_sendstr(request, response);
}

/**
 * @brief 返回内置配网页面。
 */
static esp_err_t index_handler(httpd_req_t *request)
{
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, s_index_html, HTTPD_RESP_USE_STRLEN);
}

/**
 * @brief 返回当前连接状态、当前 SSID 和 AP/STA 地址。
 */
static esp_err_t status_handler(httpd_req_t *request)
{
    wifi_provisioning_status_t status;
    esp_err_t error = wifi_provisioning_get_status(&status);
    if (error != ESP_OK) {
        return send_result(request, "500 Internal Server Error", false, esp_err_to_name(error));
    }

    char escaped_ssid[WIFI_PROVISIONING_JSON_STRING_SIZE];
    char response[512];
    char station_ip[IP4ADDR_STRLEN_MAX] = "";
    char access_point_ip[IP4ADDR_STRLEN_MAX] = "";
    json_escape(status.current_ssid, escaped_ssid, sizeof(escaped_ssid));
    if (status.sta_connected) {
        esp_ip4addr_ntoa(&status.sta_ip_info.ip, station_ip, sizeof(station_ip));
    }
    esp_ip4addr_ntoa(&status.ap_ip_info.ip, access_point_ip, sizeof(access_point_ip));

    snprintf(response,
             sizeof(response),
             "{\"state\":%d,\"connected\":%s,\"current_ssid\":\"%s\","
             "\"sta_ip\":\"%s\",\"ap_ip\":\"%s\",\"attempt_count\":%" PRIu32 "}",
             status.state,
             status.sta_connected ? "true" : "false",
             escaped_ssid,
             station_ip,
             access_point_ip,
             status.attempt_count);
    httpd_resp_set_type(request, "application/json; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_sendstr(request, response);
}

/**
 * @brief 扫描附近 Wi-Fi 并返回不包含密码的 JSON 列表。
 */
static esp_err_t scan_handler(httpd_req_t *request)
{
    wifi_ap_record_t records[WIFI_PROVISIONING_MAX_SCAN_RESULTS] = {0};
    uint16_t record_count = 0U;
    esp_err_t error = wifi_provisioning_scan(records,
                                             WIFI_PROVISIONING_MAX_SCAN_RESULTS,
                                             &record_count);
    if (error != ESP_OK) {
        return send_result(request, "503 Service Unavailable", false, esp_err_to_name(error));
    }

    httpd_resp_set_type(request, "application/json; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_send_chunk(request, "{\"networks\":[", HTTPD_RESP_USE_STRLEN);
    for (uint16_t index = 0U; index < record_count; ++index) {
        char escaped_ssid[WIFI_PROVISIONING_JSON_STRING_SIZE];
        char item[320];
        json_escape((const char *)records[index].ssid, escaped_ssid, sizeof(escaped_ssid));
        snprintf(item,
                 sizeof(item),
                 "%s{\"ssid\":\"%s\",\"rssi\":%d,\"channel\":%u,\"auth\":\"%s\"}",
                 index == 0U ? "" : ",",
                 escaped_ssid,
                 records[index].rssi,
                 records[index].primary,
                 auth_mode_to_text(records[index].authmode));
        httpd_resp_send_chunk(request, item, HTTPD_RESP_USE_STRLEN);
    }
    httpd_resp_send_chunk(request, "]}", HTTPD_RESP_USE_STRLEN);
    return httpd_resp_send_chunk(request, NULL, 0U);
}

/**
 * @brief 返回已保存的 SSID 列表，明确不向网页发送密码。
 */
static esp_err_t saved_handler(httpd_req_t *request)
{
    wifi_provisioning_profile_t profiles[WIFI_PROVISIONING_MAX_PROFILES] = {0};
    size_t profile_count = 0U;
    esp_err_t error = wifi_provisioning_get_profiles(profiles,
                                                      WIFI_PROVISIONING_MAX_PROFILES,
                                                      &profile_count);
    if (error != ESP_OK) {
        return send_result(request, "500 Internal Server Error", false, esp_err_to_name(error));
    }

    httpd_resp_set_type(request, "application/json; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_send_chunk(request, "{\"networks\":[", HTTPD_RESP_USE_STRLEN);
    for (size_t index = 0U; index < profile_count; ++index) {
        char escaped_ssid[WIFI_PROVISIONING_JSON_STRING_SIZE];
        char item[WIFI_PROVISIONING_JSON_STRING_SIZE + 32U];
        json_escape(profiles[index].ssid, escaped_ssid, sizeof(escaped_ssid));
        snprintf(item,
                 sizeof(item),
                 "%s{\"ssid\":\"%s\"}",
                 index == 0U ? "" : ",",
                 escaped_ssid);
        httpd_resp_send_chunk(request, item, HTTPD_RESP_USE_STRLEN);
    }
    httpd_resp_send_chunk(request, "]}", HTTPD_RESP_USE_STRLEN);
    return httpd_resp_send_chunk(request, NULL, 0U);
}

/**
 * @brief 接收网页提交的 SSID 和密码，持久化后异步连接。
 */
static esp_err_t connect_handler(httpd_req_t *request)
{
    char form[WIFI_PROVISIONING_FORM_BUFFER_SIZE];
    wifi_provisioning_profile_t profile = {0};
    esp_err_t error = receive_request_body(request, form, sizeof(form));
    if (error == ESP_OK) {
        error = get_form_value(form, "ssid", profile.ssid, sizeof(profile.ssid), true);
    }
    if (error == ESP_OK) {
        error = get_form_value(form, "password", profile.password, sizeof(profile.password), false);
    }
    if (error != ESP_OK || profile.ssid[0] == '\0') {
        return send_result(request, "400 Bad Request", false, "SSID 或密码格式无效");
    }

    error = wifi_provisioning_save_profile(&profile);
    if (error != ESP_OK) {
        return send_result(request, "500 Internal Server Error", false, esp_err_to_name(error));
    }
    return send_result(request, "202 Accepted", true, "配置已保存并开始连接");
}

/**
 * @brief 删除网页指定的已保存 SSID。
 */
static esp_err_t delete_handler(httpd_req_t *request)
{
    char form[WIFI_PROVISIONING_FORM_BUFFER_SIZE];
    char ssid[sizeof(((wifi_provisioning_profile_t *)0)->ssid)] = {0};
    esp_err_t error = receive_request_body(request, form, sizeof(form));
    if (error == ESP_OK) {
        error = get_form_value(form, "ssid", ssid, sizeof(ssid), true);
    }
    if (error != ESP_OK || ssid[0] == '\0') {
        return send_result(request, "400 Bad Request", false, "SSID 格式无效");
    }

    error = wifi_provisioning_delete_profile(ssid);
    if (error != ESP_OK) {
        return send_result(request, "500 Internal Server Error", false, esp_err_to_name(error));
    }
    return send_result(request, "200 OK", true, "已删除保存的 Wi-Fi");
}

esp_err_t wifi_provisioning_web_start(httpd_handle_t *server)
{
    if (server == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = CONFIG_WIFI_PROVISIONING_HTTP_PORT;
    config.max_uri_handlers = 8U + s_extension_count;
    config.stack_size = 8192;
    config.lru_purge_enable = true;

    esp_err_t error = httpd_start(server, &config);
    if (error != ESP_OK) {
        return error;
    }

    const httpd_uri_t handlers[] = {
        {.uri = "/", .method = HTTP_GET, .handler = index_handler, .user_ctx = NULL},
        {.uri = "/api/status", .method = HTTP_GET, .handler = status_handler, .user_ctx = NULL},
        {.uri = "/api/scan", .method = HTTP_GET, .handler = scan_handler, .user_ctx = NULL},
        {.uri = "/api/saved", .method = HTTP_GET, .handler = saved_handler, .user_ctx = NULL},
        {.uri = "/api/connect", .method = HTTP_POST, .handler = connect_handler, .user_ctx = NULL},
        {.uri = "/api/delete", .method = HTTP_POST, .handler = delete_handler, .user_ctx = NULL},
    };

    for (size_t index = 0U; index < sizeof(handlers) / sizeof(handlers[0]); ++index) {
        error = httpd_register_uri_handler(*server, &handlers[index]);
        if (error != ESP_OK) {
            httpd_stop(*server);
            *server = NULL;
            ESP_LOGE(TAG, "注册网页接口 %s 失败：%s", handlers[index].uri, esp_err_to_name(error));
            return error;
        }
    }

    for (size_t index = 0; index < s_extension_count; ++index) {
        error = httpd_register_uri_handler(*server, &s_extensions[index]);
        if (error != ESP_OK) {
            httpd_stop(*server);
            *server = NULL;
            return error;
        }
    }
    s_web_started = true;
    return ESP_OK;
}
