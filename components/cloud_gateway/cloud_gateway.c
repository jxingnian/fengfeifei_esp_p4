#include "cloud_gateway_internal.h"
#include <ctype.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "wifi_provisioning.h"

static const char *TAG = "cloud_gateway";

/* Only worker owns client/storage. Callback copies bounded messages to queue.
 * Epochs prevent queued traffic from an old connection reaching a new session. */
typedef enum { CMD_CONFIG, CMD_FORGET, CMD_PAIR, CMD_SEND, EV_OPEN, EV_CLOSE, EV_DATA } item_type_t;
typedef struct {
    item_type_t type;
    unsigned epoch;
    bool binary;
    size_t length;
    void *data;
} item_t;
static QueueHandle_t s_queue;
static SemaphoreHandle_t s_mutex;
static cloud_gateway_status_t s_status, s_view;
static cloud_gateway_credentials_t s_credentials;
static cloud_gateway_config_t s_config;
static esp_websocket_client_handle_t s_client;
static unsigned s_epoch;
static atomic_bool s_overflow;
static bool s_open, s_restart, s_halted, s_immediate;
static int64_t s_deadline, s_next;
static unsigned s_backoff = 1;
/* Used exclusively by the websocket callback, reset after destroy joins it. */
static char *s_rx;
static size_t s_rx_length;
static int s_rx_opcode;

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

bool cloud_gateway_valid_url(const char *url)
{
    if (!url || strlen(url) >= CLOUD_GATEWAY_URL_SIZE) return false;
    const char *host = NULL;
    if (!strncmp(url, "https://", 8)) host = url + 8;
#ifdef CONFIG_CLOUD_GATEWAY_ALLOW_INSECURE
    else if (!strncmp(url, "http://", 7)) host = url + 7;
#endif
    if (!host || !*host) return false;
    /* Origin only; reject userinfo, paths, queries and header/control injection.
     * DNS/IPv4 plus bracketed IPv6, with an optional numeric TCP port. */
    const char *end = host;
    if (*host == '[') {
        end = strchr(host, ']');
        if (!end || end == host + 1) return false;
        bool colon = false;
        for (const char *p = host + 1; p < end; ++p) {
            if (*p == ':') colon = true;
            else if (!isxdigit((unsigned char)*p) && *p != '.') return false;
        }
        if (!colon) return false;
        ++end;
    } else {
        while (*end && *end != ':') {
            if (!(isalnum((unsigned char)*end) || *end == '.' || *end == '-')) return false;
            ++end;
        }
        if (end == host || !isalnum((unsigned char)*host) ||
            !(isalnum((unsigned char)end[-1]) || end[-1] == '.')) return false;
    }
    if (*end) {
        if (*end++ != ':' || !*end) return false;
        unsigned port = 0;
        while (*end) {
            if (*end < '0' || *end > '9') return false;
            port = port * 10 + (*end++ - '0');
            if (port > 65535) return false;
        }
        if (!port) return false;
    }
    return true;
}

static void publish(cloud_gateway_state_t state, const char *message)
{
    s_status.state = state;
    s_status.paired = s_credentials.auth_code[0] != 0;
    s_status.connected = s_open;
    s_status.connection_epoch = s_epoch;
    strlcpy(s_status.server_url, s_credentials.server_url, sizeof(s_status.server_url));
    strlcpy(s_status.tenant_id, s_credentials.tenant_id, sizeof(s_status.tenant_id));
    strlcpy(s_status.device_id, s_credentials.device_id, sizeof(s_status.device_id));
    if (message) strlcpy(s_status.message, message, sizeof(s_status.message));
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_view = s_status;
    xSemaphoreGive(s_mutex);
}

esp_err_t cloud_gateway_get_status(cloud_gateway_status_t *status)
{
    if (!status) return ESP_ERR_INVALID_ARG;
    if (!s_queue) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *status = s_view;
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

static esp_err_t enqueue(item_type_t type, bool binary, const void *data, size_t length)
{
    if (!s_queue) return ESP_ERR_INVALID_STATE;
    item_t item = {.type = type, .binary = binary, .length = length};
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    item.epoch = s_epoch;
    bool ready = s_view.state == CLOUD_GATEWAY_READY;
    xSemaphoreGive(s_mutex);
    if (type == CMD_SEND && !ready) return ESP_ERR_INVALID_STATE;
    if (length) {
        item.data = malloc(length);
        if (!item.data) return ESP_ERR_NO_MEM;
        memcpy(item.data, data, length);
    }
    if (xQueueSend(s_queue, &item, 0) != pdTRUE) {
        free(item.data);
        ESP_LOGW(TAG, "后台上行队列已满：type=%d，length=%u", (int)type, (unsigned)length);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t cloud_gateway_configure(const char *url)
{
    if (!cloud_gateway_valid_url(url))
        return ESP_ERR_INVALID_ARG;
    cloud_gateway_status_t status;
    if (cloud_gateway_get_status(&status) != ESP_OK) return ESP_ERR_INVALID_STATE;
    if (status.paired && strcmp(status.server_url, url))
        return ESP_ERR_INVALID_STATE;
    cloud_gateway_credentials_t value = {.version = 1};
    strlcpy(value.server_url, url, sizeof(value.server_url));
    return enqueue(CMD_CONFIG, false, &value, sizeof(value));
}

/* 校验并排队发送 6 位后台配对码，设备不会携带客户或用户归属。 */
esp_err_t cloud_gateway_pair(const char *pairing_code)
{
    if (!pairing_code || strlen(pairing_code) != 6) return ESP_ERR_INVALID_ARG;
    for (size_t i = 0; i < 6; ++i)
        if (pairing_code[i] < '0' || pairing_code[i] > '9') return ESP_ERR_INVALID_ARG;
    return enqueue(CMD_PAIR, false, pairing_code, 7);
}

esp_err_t cloud_gateway_forget(void) { return enqueue(CMD_FORGET, false, NULL, 0); }

esp_err_t cloud_gateway_send(bool binary, const void *data, size_t length)
{
    if (!data || !length || length > CLOUD_GATEWAY_MAX_MESSAGE) return ESP_ERR_INVALID_ARG;
    return enqueue(CMD_SEND, binary, data, length);
}

static void ws_event(void *arg, esp_event_base_t base, int32_t id, void *event)
{
    (void)base;
    item_t item = {.epoch = (unsigned)(uintptr_t)arg};
    if (id == WEBSOCKET_EVENT_CONNECTED) item.type = EV_OPEN;
    else if (id == WEBSOCKET_EVENT_DISCONNECTED || id == WEBSOCKET_EVENT_ERROR) item.type = EV_CLOSE;
    else if (id == WEBSOCKET_EVENT_DATA) {
        esp_websocket_event_data_t *d = event;
        if (d->op_code >= 8) return; /* Control frames are handled by the client. */
        if (d->data_len < 0 || d->payload_len < 0 || d->payload_offset < 0) {
            atomic_store(&s_overflow, true);
            return;
        }
        if (d->payload_offset == 0 && d->op_code != 0) {
            free(s_rx);
            s_rx = malloc(CLOUD_GATEWAY_MAX_MESSAGE + 1);
            s_rx_length = 0;
            s_rx_opcode = d->op_code;
        }
        if (!s_rx || (size_t)d->data_len > CLOUD_GATEWAY_MAX_MESSAGE - s_rx_length) {
            atomic_store(&s_overflow, true);
            return;
        }
        memcpy(s_rx + s_rx_length, d->data_ptr, d->data_len);
        s_rx_length += d->data_len;
        if (!d->fin || d->payload_offset + d->data_len != d->payload_len) return;
        s_rx[s_rx_length] = 0;
        item.type = EV_DATA;
        item.binary = s_rx_opcode == 2;
        item.data = s_rx;
        item.length = s_rx_length;
        s_rx = NULL;
        s_rx_length = 0;
    } else return;
    if (xQueueSend(s_queue, &item, pdMS_TO_TICKS(100)) != pdTRUE) {
        free(item.data);
        ESP_LOGE(TAG, "后台下行接收队列已满，准备重建 WebSocket：binary=%d，length=%u",
                 item.binary ? 1 : 0, (unsigned)item.length);
        atomic_store(&s_overflow, true);
    }
}

static void disconnect_client(void)
{
    s_open = false;
    /* Invalidate queued data before destroying this connection. */
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    ++s_epoch;
    s_view.connected = false;
    s_view.state = CLOUD_GATEWAY_CONNECTING;
    s_view.connection_epoch = s_epoch;
    xSemaphoreGive(s_mutex);
    if (s_client) {
        esp_websocket_client_stop(s_client);
        esp_websocket_client_destroy(s_client);
        s_client = NULL;
    }
    free(s_rx);
    s_rx = NULL;
    s_rx_length = 0;
    atomic_store(&s_overflow, false);
    s_deadline = 0;
}

static bool send_text(const char *text)
{
    return s_client && s_open && esp_websocket_client_send_text(s_client, text, strlen(text),
                                                               pdMS_TO_TICKS(2000)) == (int)strlen(text);
}

static void send_hello(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "hello");
    cJSON_AddStringToObject(root, "deviceUid", s_status.device_uid);
    cJSON_AddStringToObject(root, "name", "FengFeifei P4");
    cJSON_AddStringToObject(root, "firmwareVersion", esp_app_get_description()->version);
    /* Hardware capability advertisement; streaming services are separate. */
    const char *capabilities[] = {"microphone", "speaker"};
    cJSON_AddItemToObject(root, "capabilities", cJSON_CreateStringArray(capabilities, 2));
    char *json = cJSON_PrintUnformatted(root);
    if (!json || !send_text(json)) s_restart = true;
    free(json);
    cJSON_Delete(root);
}

/* 设备网页输入配对码后，通过当前临时连接提交给后台；后台根据配对码确定客户归属。 */
static void send_pair_code(const char *pairing_code)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "pair");
    cJSON_AddStringToObject(root, "code", pairing_code);
    char *json = cJSON_PrintUnformatted(root);
    if (!json || !send_text(json)) s_restart = true;
    free(json);
    cJSON_Delete(root);
}

static const char *string_field(cJSON *root, const char *key)
{
    cJSON *value = cJSON_GetObjectItemCaseSensitive(root, key);
    return cJSON_IsString(value) ? value->valuestring : "";
}

static bool credential_field(const char *value, size_t capacity)
{
    size_t length = strlen(value);
    if (!length || length >= capacity) return false;
    for (size_t i = 0; i < length; ++i)
        if (!(isalnum((unsigned char)value[i]) || value[i] == '-' || value[i] == '_')) return false;
    return true;
}

static void receive_message(const item_t *item)
{
    if (!item->binary) {
        cJSON *root = cJSON_ParseWithLength(item->data, item->length);
        if (!root) { s_restart = true; return; }
        const char *type = string_field(root, "type");
        if (!s_credentials.auth_code[0] && !strcmp(type, "pairing_pending")) {
            s_deadline = 0;
            s_backoff = 1;
            publish(CLOUD_GATEWAY_WAIT_CODE, "已连接后台，请在后台或小程序查看配对码，再在此页面输入");
        } else if (!s_credentials.auth_code[0] &&
                   !strcmp(type, "paired")) {
            const char *id = string_field(root, "deviceId"), *auth = string_field(root, "deviceAuthCode");
            const char *tenant = string_field(root, "tenantId");
            if (credential_field(id, sizeof(s_credentials.device_id)) &&
                credential_field(auth, sizeof(s_credentials.auth_code))) {
                cloud_gateway_credentials_t next = s_credentials;
                strlcpy(next.device_id, id, sizeof(next.device_id));
                strlcpy(next.auth_code, auth, sizeof(next.auth_code));
                if (tenant[0] && credential_field(tenant, sizeof(next.tenant_id))) strlcpy(next.tenant_id, tenant, sizeof(next.tenant_id));
                if (cloud_gateway_storage_save(&next) == ESP_OK) {
                    s_credentials = next;
                    s_immediate = true;
                    publish(CLOUD_GATEWAY_AUTHORIZED, "授权已保存，正在建立正式连接");
                } else {
                    s_halted = true;
                    publish(CLOUD_GATEWAY_ERROR, "授权保存失败，请在后台撤销此次授权后重新配对");
                }
                memset(&next, 0, sizeof(next));
            } else {
                s_halted = true;
                publish(CLOUD_GATEWAY_ERROR, "后台返回的授权格式无效");
            }
            s_restart = true;
        } else if (s_credentials.auth_code[0] && !strcmp(type, "ready")) {
            s_deadline = 0;
            s_backoff = 1;
            publish(CLOUD_GATEWAY_READY, "后台连接正常，设备已授权");
        } else if (!strcmp(type, "error")) {
            const char *code = string_field(root, "code");
            if (s_credentials.auth_code[0] && !strcmp(code, "AUTH_REVOKED")) {
                cloud_gateway_credentials_t next = s_credentials;
                memset(next.device_id, 0, sizeof(next.device_id));
                memset(next.auth_code, 0, sizeof(next.auth_code));
                if (cloud_gateway_storage_save(&next) == ESP_OK) {
                    s_credentials = next;
                    s_immediate = true;
                }
                else s_halted = true;
                memset(&next, 0, sizeof(next));
                s_restart = true;
                publish(CLOUD_GATEWAY_ERROR, "授权已撤销，请重新配对；若清除失败请解除本地授权");
            } else if (!strcmp(code, "AUTH_FAILED")) {
                s_halted = true;
                s_restart = true;
                publish(CLOUD_GATEWAY_ERROR, "认证失败，授权仍保留；请检查后台客户状态后重新连接");
            } else {
                bool waiting_code = !s_credentials.auth_code[0] && s_status.state == CLOUD_GATEWAY_WAIT_CODE;
                if (waiting_code) s_deadline = 0;
                publish(waiting_code ? CLOUD_GATEWAY_WAIT_CODE : s_status.state,
                        string_field(root, "message"));
            }
        }
        cJSON_Delete(root);
    }
    if (s_status.state == CLOUD_GATEWAY_READY && !s_restart && s_config.on_message)
        s_config.on_message(item->binary, item->data, item->length, s_config.context);
}

static void process_item(item_t *item)
{
    if (item->type == CMD_CONFIG) {
        cloud_gateway_credentials_t *next = item->data;
        if (s_credentials.auth_code[0]) {
            if (strcmp(next->server_url, s_credentials.server_url) ||
                strcmp(next->tenant_id, s_credentials.tenant_id)) {
                publish(s_status.state, "请先解除本地授权，再更换服务器或客户");
                return;
            }
        } else if (cloud_gateway_storage_save(next) == ESP_OK) s_credentials = *next;
        else { publish(s_status.state, "配置保存失败"); return; }
        s_halted = false;
        s_immediate = true;
        s_restart = true;
    } else if (item->type == CMD_FORGET) {
        cloud_gateway_credentials_t next = s_credentials;
        memset(next.device_id, 0, sizeof(next.device_id));
        memset(next.auth_code, 0, sizeof(next.auth_code));
        if (cloud_gateway_storage_save(&next) == ESP_OK) {
            s_credentials = next;
            s_halted = false;
            s_immediate = true;
            s_restart = true;
            publish(CLOUD_GATEWAY_CONNECTING, "本地授权已清除，后台记录需由管理员撤销");
        } else publish(s_status.state, "清除本地授权失败");
    } else if (item->epoch != s_epoch || s_restart) return;
    else if (item->type == CMD_PAIR) {
        /* 配对码只能在临时连接已建立且设备尚未授权时提交。 */
        if (s_credentials.auth_code[0] || !s_open) publish(s_status.state, "当前状态不允许提交配对码");
        else {
            send_pair_code((const char *)item->data);
            publish(CLOUD_GATEWAY_PAIRING, "配对码已提交，等待后台确认");
        }
    } else if (item->type == EV_OPEN) {
        s_open = true;
        s_deadline = now_ms() + 30000;
        publish(CLOUD_GATEWAY_CONNECTING, "服务器已连接，等待确认");
        if (!s_credentials.auth_code[0]) send_hello();
    } else if (item->type == EV_CLOSE) s_restart = true;
    else if (item->type == EV_DATA) receive_message(item);
    else if (item->type == CMD_SEND && s_status.state == CLOUD_GATEWAY_READY) {
        int sent = item->binary
            ? esp_websocket_client_send_bin(s_client, item->data, item->length, pdMS_TO_TICKS(2000))
            : esp_websocket_client_send_text(s_client, item->data, item->length, pdMS_TO_TICKS(2000));
        if (sent != (int)item->length) {
            ESP_LOGE(TAG, "后台上行 WebSocket 发送不完整：binary=%d，expected=%u，sent=%d",
                     item->binary ? 1 : 0, (unsigned)item->length, sent);
            s_restart = true;
        }
    }
}

static void connect_client(void)
{
    /* 预留协议、路径和编译器可见的完整源字符串上限，确保 URL 不被截断。 */
    char url[512], headers[256] = "";
    bool secure = !strncmp(s_credentials.server_url, "https://", 8);
    snprintf(url, sizeof(url), "%s://%s/ws/device%s", secure ? "wss" : "ws",
             s_credentials.server_url + (secure ? 8 : 7), s_credentials.auth_code[0] ? "" : "/pair");
    if (s_credentials.auth_code[0])
        snprintf(headers, sizeof(headers), "X-Device-Id: %s\r\nAuthorization: Bearer %s\r\n",
                 s_credentials.device_id, s_credentials.auth_code);
    /*
     * ESP-IDF 的 WebSocket 传输层会区分“没有自定义 Header”和“存在一个空 Header”。
     * 空字符串会被传输层当作待拼接 Header，snprintf() 返回 0 后被错误判定为握手请求生成失败。
     * 未授权的配对连接没有自定义 Header，因此必须传入 NULL；已授权连接继续传入实际 Header。
     */
    const char *headers_arg = headers[0] ? headers : NULL;
    esp_websocket_client_config_t config = {
        .uri = url, .headers = headers_arg, .disable_auto_reconnect = true,
        .network_timeout_ms = 10000, .buffer_size = 2048, .task_stack = 6144,
        .ping_interval_sec = 20,
        .crt_bundle_attach = secure ? esp_crt_bundle_attach : NULL,
    };
    s_client = esp_websocket_client_init(&config);
    if (!s_client ||
        esp_websocket_register_events(s_client, WEBSOCKET_EVENT_ANY, ws_event, (void *)(uintptr_t)s_epoch) != ESP_OK ||
        esp_websocket_client_start(s_client) != ESP_OK) { s_restart = true; return; }
    s_deadline = now_ms() + 30000;
    publish(CLOUD_GATEWAY_CONNECTING, "正在连接后台");
}

static void worker(void *arg)
{
    (void)arg;
    bool time_started = false;
    for (;;) {
        item_t item;
        if (xQueueReceive(s_queue, &item, pdMS_TO_TICKS(200)) == pdTRUE) {
            process_item(&item);
            if (item.data) { memset(item.data, 0, item.length); free(item.data); }
        }
        bool network = wifi_provisioning_is_connected();
        if (atomic_load(&s_overflow) || (s_deadline && now_ms() >= s_deadline)) s_restart = true;
        if (s_restart || (!network && s_client)) {
            disconnect_client();
            s_restart = false;
            s_status.retry_seconds = s_immediate ? 0 : s_backoff;
            s_next = now_ms() + (s_immediate ? 0 : s_backoff * 1000 + esp_random() % 1000);
            s_backoff = s_immediate ? 1 : s_backoff < 30 ? s_backoff * 2 : 60;
            s_immediate = false;
            publish(s_halted ? CLOUD_GATEWAY_ERROR : CLOUD_GATEWAY_RETRY,
                    s_halted ? NULL : "连接已断开，将自动重连；未完成配对时请在设备网页重新输入后台生成的配对码");
        }
        if (s_halted) continue;
        if (!network) { publish(CLOUD_GATEWAY_WAIT_NETWORK, "请先连接 Wi-Fi"); continue; }
        if (!time_started) {
            esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
            esp_err_t result = esp_netif_sntp_init(&config);
            time_started = result == ESP_OK || result == ESP_ERR_INVALID_STATE;
        }
        if (!strncmp(s_credentials.server_url, "https://", 8) && time(NULL) < 1735689600) {
            publish(CLOUD_GATEWAY_SYNC_TIME, "正在同步时间以验证 TLS 证书，请确保网络允许 NTP");
            continue;
        }
        if (!s_client && now_ms() >= s_next) {
            s_status.retry_seconds = 0;
            connect_client();
        }
    }
}

esp_err_t cloud_gateway_start(const cloud_gateway_config_t *config)
{
    if (s_queue) return ESP_ERR_INVALID_STATE;
    esp_err_t storage_error = cloud_gateway_storage_load(&s_credentials);
    if (storage_error != ESP_OK) {
        /* Keep local recovery UI usable; never authenticate using a corrupt blob. */
        memset(&s_credentials, 0, sizeof(s_credentials));
        s_credentials.version = 1;
        strlcpy(s_credentials.server_url, CONFIG_CLOUD_GATEWAY_SERVER_URL, sizeof(s_credentials.server_url));
        s_halted = true;
    }
    uint8_t mac[6];
    esp_err_t error = esp_efuse_mac_get_default(mac);
    if (error != ESP_OK) return error;
    snprintf(s_status.device_uid, sizeof(s_status.device_uid), "p4c5-%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    s_mutex = xSemaphoreCreateMutex();
    s_queue = xQueueCreate(CLOUD_GATEWAY_QUEUE_LENGTH, sizeof(item_t));
    if (!s_mutex || !s_queue) {
        if (s_mutex) vSemaphoreDelete(s_mutex);
        if (s_queue) vQueueDelete(s_queue);
        s_mutex = NULL;
        s_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    if (config) s_config = *config;
    publish(s_halted ? CLOUD_GATEWAY_ERROR : CLOUD_GATEWAY_WAIT_NETWORK,
            s_halted ? "授权存储读取失败，请检查配置后重新连接；旧授权需在后台撤销" : "等待 Wi-Fi 联网");
    if (xTaskCreate(worker, "cloud_gateway", 8192, NULL, 5, NULL) != pdPASS) {
        vQueueDelete(s_queue);
        vSemaphoreDelete(s_mutex);
        s_queue = NULL;
        s_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
