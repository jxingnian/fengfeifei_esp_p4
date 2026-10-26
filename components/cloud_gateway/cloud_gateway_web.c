#include "cloud_gateway.h"
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "wifi_provisioning.h"

/* JSON-only writes deliberately require a custom header: a remote website
 * cannot submit local control commands with a cross-origin HTML form. */
static const char s_cloud_html[] =
    "<!doctype html><html lang='zh-CN'><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'><title>后台连接与设备配对</title>"
    "<style>body{font:15px Arial,'Microsoft Yahei';margin:0;color:#1f2937;background:#fff;border-radius:14px}"
    ".box{padding:18px}h2{font-size:18px;margin:0 0 12px}input,button{width:100%;box-sizing:border-box;padding:10px;margin:5px 0;border:1px solid #d1d5db;border-radius:7px;font-size:15px}"
    "button{background:#2563eb;color:#fff;border:0;cursor:pointer}button:disabled{opacity:.5;cursor:default}.danger{background:#dc2626}"
    ".grid{display:grid;grid-template-columns:95px minmax(0,1fr);gap:8px;font-size:13px}b{overflow-wrap:anywhere}"
    ".muted{color:#64748b;font-size:12px;line-height:1.5}.msg{min-height:40px;color:#2563eb;font-size:13px;margin-top:8px}</style>"
    "<div class='box'><h2>后台连接与设备配对</h2><div class='grid'>"
    "<span>设备 UID</span><b id='uid'>-</b><span>连接状态</span><b id='cloudState'>读取中</b>"
    "<span>配对状态</span><b id='paired'>-</b><span>设备 ID</span><b id='did'>-</b>"
    "<span>待配对 ID</span><b id='pid'>-</b><span>设备授权码</span><b id='pairingCode'>-</b></div>"
    "<label>服务器地址<input id='serverUrl' maxlength='191' placeholder='https://iot.xingnian.vip'></label>"
    "<label>客户 ID（选填）<input id='tenantId' maxlength='64' placeholder='后台组织 ID；不填时由平台管理员配对'></label>"
    "<button id='connectButton' onclick='configureCloud()'>连接后台 / 重新连接</button>"
    "<button id='forgetButton' class='danger' onclick='forgetCloud()'>解除本地授权</button>"
    "<div id='cloudMessage' class='msg' role='status'></div><div class='muted'>"
    "先连接 Wi-Fi，再连接后台。设备会自动生成授权码并显示在此处，请在后台或小程序输入该授权码完成配对。"
    "授权成功后设备会自动保存长期凭证并连接正式服务。</div></div>"
    "<script>const el=id=>document.getElementById(id);let initialized=false,busy=false;"
    "const states=['等待 Wi-Fi','同步时间','连接中','等待配对码','验证配对码','已授权','后台就绪','重连等待','错误'];"
    "async function request(path,data){const options=data===undefined?{}:{method:'POST',headers:{'Content-Type':'application/json','X-Cloud-Control':'1'},body:JSON.stringify(data)};"
    "const r=await fetch(path,options),j=await r.json();if(!r.ok)throw Error(j.message||'请求失败');return j;}"
    "async function refresh(){try{const s=await request('/api/cloud/status');el('uid').textContent=s.deviceUid;el('cloudState').textContent=states[s.state]||'未知';"
    "el('paired').textContent=s.paired?'已配对':'未配对';el('did').textContent=s.deviceId||'-';el('pid').textContent=s.pairingId||'-';el('pairingCode').textContent=s.pairingCode||'-';"
    "el('serverUrl').disabled=s.paired||busy;el('tenantId').disabled=s.paired||busy;"
    "if(!initialized){el('serverUrl').value=s.serverUrl;el('tenantId').value=s.tenantId;initialized=true;}"
    "if(!busy)el('cloudMessage').textContent=s.message;}catch(e){el('cloudMessage').textContent=e.message;}}"
    "async function command(path,data){if(busy)return;busy=true;el('connectButton').disabled=true;el('forgetButton').disabled=true;"
    "try{await request(path,data);el('cloudMessage').textContent='请求已提交，等待设备处理';}catch(e){el('cloudMessage').textContent=e.message;}"
    "finally{busy=false;el('connectButton').disabled=false;el('forgetButton').disabled=false;}}"
    "function configureCloud(){command('/api/cloud/configure',{serverUrl:el('serverUrl').value.trim().replace(/\\/+$/,''),tenantId:el('tenantId').value.trim()});}"
    "function forgetCloud(){if(confirm('确认清除本机授权？后台记录和权限还需管理员撤销。'))command('/api/cloud/forget',{});}"
    "async function poll(){await refresh();setTimeout(poll,2000);}poll();</script></html>";

static esp_err_t send_json(httpd_req_t *request, const char *status, cJSON *root)
{
    if (!root) return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    httpd_resp_set_status(request, status);
    httpd_resp_set_type(request, "application/json; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    esp_err_t result = httpd_resp_sendstr(request, json);
    free(json);
    return result;
}

static esp_err_t result(httpd_req_t *request, esp_err_t error)
{
    const char *message = error == ESP_OK ? "请求已加入处理队列" :
        error == ESP_ERR_INVALID_STATE ? "设备状态不允许此操作，请等待连接或先解除本地授权" :
        error == ESP_ERR_INVALID_ARG ? "服务器地址、客户 ID 或配对码格式无效" :
        error == ESP_ERR_TIMEOUT ? "设备忙，请稍后重试" : esp_err_to_name(error);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "message", message);
    return send_json(request, error == ESP_OK ? "202 Accepted" : "400 Bad Request", root);
}

static cJSON *read_json(httpd_req_t *request)
{
    char header[64], buffer[768];
    if (httpd_req_get_hdr_value_str(request, "X-Cloud-Control", header, sizeof(header)) != ESP_OK ||
        strcmp(header, "1") ||
        httpd_req_get_hdr_value_str(request, "Content-Type", header, sizeof(header)) != ESP_OK ||
        strcmp(header, "application/json") || request->content_len == 0 ||
        request->content_len >= sizeof(buffer)) return NULL;
    size_t total = 0;
    while (total < request->content_len) {
        int received = httpd_req_recv(request, buffer + total, request->content_len - total);
        if (received <= 0) return NULL; /* Bound slow-client timeouts. */
        total += received;
    }
    buffer[total] = 0;
    cJSON *root = cJSON_ParseWithLength(buffer, total);
    if (!cJSON_IsObject(root)) { cJSON_Delete(root); return NULL; }
    return root;
}

static const char *field(cJSON *root, const char *name)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

static esp_err_t index_handler(httpd_req_t *request)
{
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_hdr(request, "X-Content-Type-Options", "nosniff");
    return httpd_resp_send(request, s_cloud_html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t status_handler(httpd_req_t *request)
{
    cloud_gateway_status_t s;
    esp_err_t error = cloud_gateway_get_status(&s);
    if (error != ESP_OK) return result(request, error);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "state", s.state);
    cJSON_AddBoolToObject(root, "paired", s.paired);
    cJSON_AddBoolToObject(root, "connected", s.connected);
    cJSON_AddStringToObject(root, "serverUrl", s.server_url);
    cJSON_AddStringToObject(root, "tenantId", s.tenant_id);
    cJSON_AddStringToObject(root, "deviceUid", s.device_uid);
    cJSON_AddStringToObject(root, "deviceId", s.device_id);
    cJSON_AddStringToObject(root, "pairingId", s.pairing_id);
    /* 临时授权码只返回给设备本地页面，长期设备授权码不会出现在这里。 */
    cJSON_AddStringToObject(root, "pairingCode", s.pairing_code);
    cJSON_AddStringToObject(root, "message", s.message);
    cJSON_AddNumberToObject(root, "retrySeconds", s.retry_seconds);
    return send_json(request, "200 OK", root);
}

static esp_err_t command_handler(httpd_req_t *request)
{
    cJSON *root = read_json(request);
    if (!root) return result(request, ESP_ERR_INVALID_ARG);
    esp_err_t error;
    if (!strcmp(request->uri, "/api/cloud/configure")) {
        const char *tenant = field(root, "tenantId");
        error = cloud_gateway_configure(field(root, "serverUrl"), tenant ? tenant : "");
    } else error = cloud_gateway_forget();
    cJSON_Delete(root);
    return result(request, error);
}

static const httpd_uri_t s_handlers[] = {
    {.uri = "/cloud", .method = HTTP_GET, .handler = index_handler},
    {.uri = "/api/cloud/status", .method = HTTP_GET, .handler = status_handler},
    {.uri = "/api/cloud/configure", .method = HTTP_POST, .handler = command_handler},
    {.uri = "/api/cloud/forget", .method = HTTP_POST, .handler = command_handler},
};

esp_err_t cloud_gateway_web_register(void)
{
    return wifi_provisioning_register_web_extension(s_handlers, sizeof(s_handlers) / sizeof(s_handlers[0]));
}
