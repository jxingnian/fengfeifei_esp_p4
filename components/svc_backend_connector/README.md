# XN Device Connector 组件

设备端后台连接器组件，用于ESP32设备与星念AI后台服务器的配对、认证和通信。

## 功能特性

- **设备配对**: 通过WebSocket配对流程，使用6位数字配对码
- **认证管理**: 安全的设备认证，凭证持久化存储在NVS
- **长连接通信**: WebSocket长连接，支持自动重连
- **消息路由**: 自动区分控制消息和音频数据
- **心跳保活**: 自动发送心跳包保持连接
- **配置同步**: 从后台获取设备配置信息

## 使用方法

### 1. 初始化组件

```c
#include "xn_device_connector.h"

// 配置连接器
xn_connector_config_t config = {
    .server_url = "ws://192.168.1.100:3000",  // 后台服务器地址
    .reconnect_interval_ms = 5000,             // 重连间隔
    .heartbeat_interval_ms = 30000,            // 心跳间隔
    .pairing_timeout_ms = 600000,              // 配对超时
    .auto_reconnect = true,                    // 自动重连
    .voice_protocol = 2                        // 语音协议版本
};

// 初始化
esp_err_t ret = xn_connector_init(&config);
if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to init connector");
}
```

### 2. 配对设备

```c
// 配对回调
void pairing_callback(xn_pairing_event_t event, const char *pairing_id, void *user_data)
{
    switch (event) {
        case XN_PAIRING_EVENT_PENDING:
            ESP_LOGI(TAG, "等待输入配对码，配对ID: %s", pairing_id);
            // 提示用户输入配对码
            break;
        case XN_PAIRING_EVENT_SUCCESS:
            ESP_LOGI(TAG, "配对成功");
            break;
        case XN_PAIRING_EVENT_FAILED:
            ESP_LOGE(TAG, "配对失败");
            break;
    }
}

// 设备信息
xn_device_info_t device_info = {
    .device_uid = "ESP32-123456",
    .name = "我的ESP32设备",
    .firmware_version = "1.0.0",
    .capabilities = (char *[]){"microphone", "speaker"},
    .capabilities_count = 2
};

// 回调集合
xn_connector_callbacks_t callbacks = {
    .pairing_cb = pairing_callback,
    .user_data = NULL
};

// 开始配对
ret = xn_connector_start_pairing(&device_info, &callbacks);

// 用户输入配对码后提交
ret = xn_connector_submit_pairing_code("123456");
```

### 3. 连接后台

```c
// 连接回调
void connection_callback(xn_conn_event_t event, void *user_data)
{
    switch (event) {
        case XN_CONN_EVENT_CONNECTED:
            ESP_LOGI(TAG, "已连接");
            break;
        case XN_CONN_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "已断开");
            break;
        case XN_CONN_EVENT_RECONNECTING:
            ESP_LOGI(TAG, "重连中");
            break;
        case XN_CONN_EVENT_ERROR:
            ESP_LOGE(TAG, "连接错误");
            break;
    }
}

// 消息回调
void message_callback(xn_msg_type_t msg_type, const uint8_t *data, size_t len, void *user_data)
{
    if (msg_type == XN_MSG_TYPE_CONTROL) {
        ESP_LOGI(TAG, "收到控制消息: %.*s", len, (char *)data);
    } else if (msg_type == XN_MSG_TYPE_AUDIO) {
        ESP_LOGI(TAG, "收到音频数据: %d bytes", len);
        // 播放音频
    }
}

// 回调集合
xn_connector_callbacks_t callbacks = {
    .conn_cb = connection_callback,
    .message_cb = message_callback,
    .user_data = NULL
};

// 连接
ret = xn_connector_connect(&callbacks);
```

### 4. 发送数据

```c
// 发送音频数据
uint8_t audio_data[1024];
ret = xn_connector_send_audio(audio_data, sizeof(audio_data));

// 发送控制消息
const char *msg = "{\"type\":\"audio_start\",\"turnId\":1}";
ret = xn_connector_send_control(msg);
```

### 5. 其他操作

```c
// 检查状态
bool is_paired = xn_connector_is_paired();
bool is_connected = xn_connector_is_connected();
xn_connector_state_t state = xn_connector_get_state();

// 获取设备ID
char device_id[64];
ret = xn_connector_get_device_id(device_id, sizeof(device_id));

// 清除配对（取消配对）
ret = xn_connector_clear_pairing();

// 断开连接
ret = xn_connector_disconnect();

// 反初始化
ret = xn_connector_deinit();
```

## API 参考

详见 `include/xn_device_connector.h` 头文件中的详细注释。

## 依赖

- nvs_flash
- esp_http_client
- json (cJSON)
- mbedtls

## 配对流程说明

1. 管理员在后台生成6位数字配对码
2. 设备调用 `xn_connector_start_pairing()` 开始配对
3. 设备连接到后台配对WebSocket并发送设备信息
4. 后台返回 `pairing_pending` 事件，触发 `XN_PAIRING_EVENT_PENDING` 回调
5. 用户在设备端输入配对码
6. 设备调用 `xn_connector_submit_pairing_code()` 提交配对码
7. 后台验证配对码，返回 `deviceId`、`tenantId` 和 `deviceAuthCode`
8. 设备保存认证信息到NVS，触发 `XN_PAIRING_EVENT_SUCCESS` 回调
9. 配对完成，设备可以使用 `xn_connector_connect()` 连接

## 认证机制

- 设备使用 `deviceId` 和 `deviceAuthCode` 进行认证
- `deviceToken` 使用 HMAC-SHA256 算法生成: `HMAC(deviceAuthCode, deviceId)` 然后转为base64url
- 认证信息安全存储在NVS分区

## 注意事项

1. 配对信息会持久化存储，重启后无需重新配对
2. 如果后台撤销设备授权，组件会自动清除本地配对信息
3. 支持自动重连，网络断开后会自动尝试重连
4. 心跳间隔建议设置为30秒
5. 音频数据使用二进制帧发送，控制消息使用文本帧发送
