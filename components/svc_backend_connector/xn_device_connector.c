#include "xn_device_connector.h"
#include "xn_connector_internal.h"
#include "esp_log.h"
#include <string.h>
#include "nvs.h"

static const char *TAG = "xn_connector";

static xn_connector_ctx_t g_connector_ctx = {0};

xn_connector_ctx_t *xn_connector_get_context(void)
{
    return &g_connector_ctx;
}

esp_err_t xn_connector_init(const xn_connector_config_t *config)
{
    if (config == NULL) {
        ESP_LOGE(TAG, "Config is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    if (g_connector_ctx.state != XN_CONNECTOR_STATE_IDLE) {
        ESP_LOGW(TAG, "Connector already initialized");
        return ESP_ERR_INVALID_STATE;
    }

    memset(&g_connector_ctx, 0, sizeof(xn_connector_ctx_t));
    memcpy(&g_connector_ctx.config, config, sizeof(xn_connector_config_t));
    g_connector_ctx.state = XN_CONNECTOR_STATE_IDLE;

    g_connector_ctx.state_mutex = xSemaphoreCreateMutex();
    if (g_connector_ctx.state_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }

    g_connector_ctx.event_group = xEventGroupCreate();
    if (g_connector_ctx.event_group == NULL) {
        ESP_LOGE(TAG, "Failed to create event group");
        vSemaphoreDelete(g_connector_ctx.state_mutex);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = xn_auth_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init auth module: %s", esp_err_to_name(ret));
        vSemaphoreDelete(g_connector_ctx.state_mutex);
        vEventGroupDelete(g_connector_ctx.event_group);
        return ret;
    }

    /* 先迁移旧版数据，再读取正式认证信息，保证已有设备无需重新配对。 */
    ret = xn_auth_migrate_legacy();
    if (ret != ESP_OK && ret != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(TAG, "旧版配对数据迁移失败: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = xn_auth_load(&g_connector_ctx.auth_info);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Loaded auth info from NVS");
        g_connector_ctx.state = XN_CONNECTOR_STATE_PAIRED;
    } else {
        ESP_LOGI(TAG, "No auth info found, device not paired");
    }

    ret = xn_message_router_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init message router: %s", esp_err_to_name(ret));
    }

    ret = xn_config_sync_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init config sync: %s", esp_err_to_name(ret));
    }

    ESP_LOGI(TAG, "Connector initialized successfully");
    return ESP_OK;
}

esp_err_t xn_connector_deinit(void)
{
    xSemaphoreTake(g_connector_ctx.state_mutex, portMAX_DELAY);

    if (g_connector_ctx.ws_client != NULL) {
        xn_connection_stop();
    }

    if (g_connector_ctx.pairing_active) {
        xn_pairing_stop();
    }

    if (g_connector_ctx.heartbeat_task != NULL) {
        vTaskDelete(g_connector_ctx.heartbeat_task);
        g_connector_ctx.heartbeat_task = NULL;
    }

    vEventGroupDelete(g_connector_ctx.event_group);
    xSemaphoreGive(g_connector_ctx.state_mutex);
    vSemaphoreDelete(g_connector_ctx.state_mutex);

    memset(&g_connector_ctx, 0, sizeof(xn_connector_ctx_t));

    ESP_LOGI(TAG, "Connector deinitialized");
    return ESP_OK;
}

esp_err_t xn_connector_start_pairing(const xn_device_info_t *device_info, const xn_connector_callbacks_t *callbacks)
{
    if (device_info == NULL || callbacks == NULL) {
        ESP_LOGE(TAG, "Invalid arguments");
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(g_connector_ctx.state_mutex, portMAX_DELAY);

    if (g_connector_ctx.pairing_active) {
        ESP_LOGW(TAG, "Pairing already in progress");
        xSemaphoreGive(g_connector_ctx.state_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    memcpy(&g_connector_ctx.callbacks, callbacks, sizeof(xn_connector_callbacks_t));
    g_connector_ctx.state = XN_CONNECTOR_STATE_PAIRING;

    xSemaphoreGive(g_connector_ctx.state_mutex);

    esp_err_t ret = xn_pairing_start(device_info);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start pairing: %s", esp_err_to_name(ret));
        xSemaphoreTake(g_connector_ctx.state_mutex, portMAX_DELAY);
        g_connector_ctx.state = XN_CONNECTOR_STATE_IDLE;
        xSemaphoreGive(g_connector_ctx.state_mutex);
        return ret;
    }

    ESP_LOGI(TAG, "Pairing started");
    return ESP_OK;
}

esp_err_t xn_connector_submit_pairing_code(const char *pairing_code)
{
    if (pairing_code == NULL) {
        ESP_LOGE(TAG, "Pairing code is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    if (!g_connector_ctx.pairing_active) {
        ESP_LOGE(TAG, "Pairing not active");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = xn_pairing_submit_code(pairing_code);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to submit pairing code: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Pairing code submitted");
    return ESP_OK;
}

esp_err_t xn_connector_stop_pairing(void)
{
    if (!g_connector_ctx.pairing_active) {
        ESP_LOGW(TAG, "Pairing not active");
        return ESP_OK;
    }

    esp_err_t ret = xn_pairing_stop();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to stop pairing: %s", esp_err_to_name(ret));
        return ret;
    }

    xSemaphoreTake(g_connector_ctx.state_mutex, portMAX_DELAY);
    g_connector_ctx.state = XN_CONNECTOR_STATE_IDLE;
    xSemaphoreGive(g_connector_ctx.state_mutex);

    ESP_LOGI(TAG, "Pairing stopped");
    return ESP_OK;
}

esp_err_t xn_connector_connect(const xn_connector_callbacks_t *callbacks)
{
    if (callbacks == NULL) {
        ESP_LOGE(TAG, "Callbacks is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    if (!g_connector_ctx.auth_info.is_valid) {
        ESP_LOGE(TAG, "Device not paired, cannot connect");
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(g_connector_ctx.state_mutex, portMAX_DELAY);

    if (g_connector_ctx.state == XN_CONNECTOR_STATE_CONNECTED ||
        g_connector_ctx.state == XN_CONNECTOR_STATE_CONNECTING) {
        ESP_LOGW(TAG, "Already connected or connecting");
        xSemaphoreGive(g_connector_ctx.state_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    memcpy(&g_connector_ctx.callbacks, callbacks, sizeof(xn_connector_callbacks_t));
    g_connector_ctx.state = XN_CONNECTOR_STATE_CONNECTING;

    xSemaphoreGive(g_connector_ctx.state_mutex);

    esp_err_t ret = xn_connection_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start connection: %s", esp_err_to_name(ret));
        xSemaphoreTake(g_connector_ctx.state_mutex, portMAX_DELAY);
        g_connector_ctx.state = XN_CONNECTOR_STATE_DISCONNECTED;
        xSemaphoreGive(g_connector_ctx.state_mutex);
        return ret;
    }

    ESP_LOGI(TAG, "Connection started");
    return ESP_OK;
}

esp_err_t xn_connector_disconnect(void)
{
    esp_err_t ret = xn_connection_stop();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to stop connection: %s", esp_err_to_name(ret));
        return ret;
    }

    xSemaphoreTake(g_connector_ctx.state_mutex, portMAX_DELAY);
    g_connector_ctx.state = XN_CONNECTOR_STATE_DISCONNECTED;
    xSemaphoreGive(g_connector_ctx.state_mutex);

    ESP_LOGI(TAG, "Disconnected");
    return ESP_OK;
}

esp_err_t xn_connector_send_audio(const uint8_t *audio_data, size_t len)
{
    if (audio_data == NULL || len == 0) {
        ESP_LOGE(TAG, "Invalid audio data");
        return ESP_ERR_INVALID_ARG;
    }

    if (g_connector_ctx.state != XN_CONNECTOR_STATE_CONNECTED) {
        ESP_LOGE(TAG, "Not connected");
        return ESP_ERR_INVALID_STATE;
    }

    return xn_connection_send_binary(audio_data, len);
}

esp_err_t xn_connector_send_control(const char *message)
{
    if (message == NULL) {
        ESP_LOGE(TAG, "Message is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    if (g_connector_ctx.state != XN_CONNECTOR_STATE_CONNECTED) {
        ESP_LOGE(TAG, "Not connected");
        return ESP_ERR_INVALID_STATE;
    }

    return xn_connection_send_text(message);
}

xn_connector_state_t xn_connector_get_state(void)
{
    xn_connector_state_t state;
    xSemaphoreTake(g_connector_ctx.state_mutex, portMAX_DELAY);
    state = g_connector_ctx.state;
    xSemaphoreGive(g_connector_ctx.state_mutex);
    return state;
}

bool xn_connector_is_paired(void)
{
    return g_connector_ctx.auth_info.is_valid;
}

bool xn_connector_is_connected(void)
{
    return g_connector_ctx.state == XN_CONNECTOR_STATE_CONNECTED;
}

esp_err_t xn_connector_clear_pairing(void)
{
    esp_err_t ret = xn_auth_clear();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to clear auth info: %s", esp_err_to_name(ret));
        return ret;
    }

    memset(&g_connector_ctx.auth_info, 0, sizeof(xn_auth_info_t));
    g_connector_ctx.auth_info.is_valid = false;

    xSemaphoreTake(g_connector_ctx.state_mutex, portMAX_DELAY);
    if (g_connector_ctx.state == XN_CONNECTOR_STATE_PAIRED) {
        g_connector_ctx.state = XN_CONNECTOR_STATE_IDLE;
    }
    xSemaphoreGive(g_connector_ctx.state_mutex);

    ESP_LOGI(TAG, "Pairing cleared");
    return ESP_OK;
}

esp_err_t xn_connector_get_device_id(char *device_id, size_t len)
{
    if (device_id == NULL || len == 0) {
        ESP_LOGE(TAG, "Invalid arguments");
        return ESP_ERR_INVALID_ARG;
    }

    if (!g_connector_ctx.auth_info.is_valid) {
        ESP_LOGE(TAG, "Device not paired");
        return ESP_ERR_INVALID_STATE;
    }

    size_t copy_len = strlen(g_connector_ctx.auth_info.device_id);
    if (copy_len >= len) {
        copy_len = len - 1;
    }

    memcpy(device_id, g_connector_ctx.auth_info.device_id, copy_len);
    device_id[copy_len] = '\0';

    return ESP_OK;
}
