# 后台配对功能实现说明

## 📋 功能概述

实现了ESP32设备与后台服务器的配对连接功能，支持设备首次启动时通过配对码绑定到后台管理系统。

---

## 🏗️ 架构设计

### 组件结构

```
components/
├── svc_backend_pairing/          # 服务层：后台配对服务
│   ├── backend_pairing.h         # 配对API
│   ├── backend_pairing.c         # WebSocket配对实现
│   ├── CMakeLists.txt
│   └── Kconfig                   # 配置选项
│
└── app_pairing_ui/               # 应用层：配对界面
    ├── pairing_ui.h              # 网页API
    ├── pairing_ui.c              # HTTP服务器实现
    └── CMakeLists.txt

main/
└── app_main.c                    # 主入口：集成配对逻辑
```

### 分层说明

- **服务层** (`svc_backend_pairing`)：负责WebSocket连接、消息收发、NVS存储
- **应用层** (`app_pairing_ui`)：负责本地HTTP服务器和网页显示
- **主入口** (`app_main.c`)：启动时检查配对状态，协调配对流程

---

## 🔄 配对流程

### 1. 设备启动检查

```c
app_main() 启动
    ↓
检查 NVS 是否有配对信息
    ↓
┌─────────────────┬─────────────────┐
│   已配对         │   未配对         │
│   跳过配对       │   启动配对流程    │
│   正常初始化     │   s_pairing_mode=true │
└─────────────────┴─────────────────┘
```

### 2. 配对流程（未配对设备）

```
ESP32启动
    ↓
启动WiFi配网
    ↓
WiFi连接成功，获取IP
    ↓
启动HTTP服务器（端口80）
    ↓
连接后台WebSocket (wss://host/ws/device/pair)
    ↓
发送 {"type":"hello", "deviceUid":"MAC", ...}
    ↓
后台返回 {"type":"pairing_code_ready", "code":"628391"}
    ↓
网页显示配对码
    ↓
用户在管理后台输入配对码
    ↓
后台返回 {"type":"paired", "deviceId":"xxx", "deviceAuthCode":"yyy"}
    ↓
保存到NVS
    ↓
显示配对成功
    ↓
3秒后自动重启
    ↓
重启后检测到已配对，进入正常运行模式
```

### 3. 已配对设备启动

```
ESP32启动
    ↓
从NVS加载配对信息
    ↓
跳过配对流程
    ↓
继续正常初始化：
    - 初始化状态机
    - 初始化音频服务
    - 初始化Coze服务（本地）
    - 初始化AI对话
    - 初始化唤醒词
    - 初始化通话组件
    ↓
正常运行
```

---

## 🔧 配置方法

### 1. 配置后台服务器地址

运行 `idf.py menuconfig`，进入：

```
Component config → Backend Pairing Configuration
```

配置项：
- **Backend Pairing WebSocket URL**: 后台配对WebSocket地址
  - 格式：`wss://host/ws/device/pair`
  - 示例：`wss://ai-server.example.com/ws/device/pair`
  - 默认：`wss://your-server.com/ws/device/pair`

- **Pairing Timeout**: 配对超时时间（毫秒）
  - 默认：300000（5分钟）
  - 范围：30000 - 600000

### 2. 编译和烧录

```bash
# 配置项目
idf.py menuconfig

# 编译
idf.py build

# 烧录
idf.py flash monitor
```

---

## 📱 使用方法

### 首次配对

1. **设备启动**
   - ESP32首次启动，未检测到配对信息
   - 自动进入配对模式
   - 启动WiFi配网（AP模式或连接已保存的WiFi）

2. **连接WiFi**
   - 如果是首次使用，连接设备的AP热点进行WiFi配网
   - 配网完成后设备自动连接WiFi

3. **访问配对网页**
   - WiFi连接成功后，ESP32会通过串口输出设备IP地址
   - 浏览器访问：`http://<设备IP>/`
   - 网页显示6位数字配对码

4. **输入配对码**
   - 在管理后台或小程序输入配对码
   - 后台系统绑定设备

5. **配对完成**
   - 设备显示"配对成功"
   - 3秒后自动重启
   - 重启后进入正常运行模式

### 已配对设备

- 设备启动时自动跳过配对流程
- 直接进入正常运行模式
- 本地AI对话功能正常工作
- 配对信息保存在NVS中，断电不丢失

### 重新配对

如需重新配对（更换后台账户或重置设备）：

1. 调用 `backend_pairing_clear_nvs()` 清除配对信息
2. 重启设备
3. 设备自动进入配对模式

---

## 🔑 NVS存储

配对信息存储在NVS中：

**命名空间**: `pairing`

**存储键**：
- `device_id` (string): 设备ID
- `auth_code` (string): 设备授权码
- `tenant_id` (string): 客户ID
- `is_paired` (uint8): 是否已配对标志

---

## 🌐 配对网页

### 访问地址

- **本地访问**: `http://<设备IP>/`
- **状态API**: `http://<设备IP>/status`

### 网页功能

- 显示配对码（大号字体）
- 显示有效期倒计时
- 实时更新配对状态
- 配对成功提示
- 配对错误提示

### 网页界面

```
┌─────────────────────────────────┐
│         设备配对                 │
├─────────────────────────────────┤
│                                 │
│         628391                  │
│     (大号字体配对码)              │
│                                 │
│   请在管理后台或小程序输入此配对码  │
│                                 │
│      有效期: 298 秒              │
│   配对码有效期5分钟，请尽快输入    │
│                                 │
│  设备地址: 192.168.1.100        │
└─────────────────────────────────┘
```

---

## 📡 后台接口

配对功能使用的后台接口（已实现）：

### 1. 配对WebSocket

**地址**: `wss://host/ws/device/pair`

**消息格式**：

设备发送：
```json
{
  "type": "hello",
  "deviceUid": "A1B2C3D4E5F6",
  "name": "ESP32-P4-AI",
  "firmwareVersion": "1.0.0",
  "capabilities": ["microphone", "speaker", "camera", "webrtc"]
}
```

后台返回配对码：
```json
{
  "type": "pairing_code_ready",
  "pairingId": "xxx",
  "code": "628391",
  "expiresAt": "2024-01-01T12:05:00Z",
  "expiresIn": 300
}
```

后台返回配对成功：
```json
{
  "type": "paired",
  "deviceId": "device-uuid-xxx",
  "tenantId": "tenant-uuid-yyy",
  "deviceAuthCode": "base64-auth-code"
}
```

后台返回错误：
```json
{
  "type": "error",
  "code": "PAIRING_FAILED",
  "message": "配对码已过期"
}
```

### 2. 管理后台接口

- `GET /api/device-pairings` - 查看待配对设备列表
- `POST /api/device-pairings/claim` - 通过配对码绑定设备

---

## 🔒 安全性

### 实现的安全措施

1. **TLS加密**: 使用 `wss://` 连接，WebSocket通信加密
2. **配对码时效**: 配对码5分钟有效，过期自动失效
3. **授权码保护**: `deviceAuthCode` 保存在NVS中，不通过网页传输
4. **一次性配对**: 配对成功后立即失效，不可重复使用

### 安全建议

1. **NVS加密**: 生产环境建议启用NVS加密存储
2. **HTTPS网页**: 配对网页建议使用HTTPS（需要配置证书）
3. **MAC地址**: deviceUid使用MAC地址，确保唯一性

---

## 🛠️ API参考

### backend_pairing.h

```c
// 初始化配对服务
esp_err_t backend_pairing_init(const backend_pairing_config_t *config);

// 开始配对
esp_err_t backend_pairing_start(void);

// 停止配对
esp_err_t backend_pairing_stop(void);

// 获取配对码
const char* backend_pairing_get_code(void);

// 检查是否已配对
bool backend_pairing_is_paired(void);

// 保存配对结果到NVS
esp_err_t backend_pairing_save_to_nvs(const backend_pairing_result_t *result);

// 从NVS加载配对信息
esp_err_t backend_pairing_load_from_nvs(backend_pairing_result_t *result);

// 清除配对信息
esp_err_t backend_pairing_clear_nvs(void);
```

### pairing_ui.h

```c
// 启动配对网页
esp_err_t pairing_ui_start(void);

// 更新配对码显示
esp_err_t pairing_ui_update_code(const char *code, int expires_in);

// 显示配对成功
esp_err_t pairing_ui_show_success(void);

// 显示配对错误
esp_err_t pairing_ui_show_error(const char *error_message);

// 停止网页服务器
esp_err_t pairing_ui_stop(void);
```

---

## ✅ 功能特点

1. **不影响现有功能**: 配对是独立模块，已配对设备直接跳过
2. **本地AI对话正常**: 配对完成后，本地Coze对话继续工作
3. **用户友好**: 网页显示配对码，操作简单直观
4. **架构清晰**: 分层设计，服务层负责通信，应用层负责显示
5. **状态持久化**: 配对信息保存在NVS，断电不丢失
6. **后期可扩展**: 保存的deviceId和authCode可用于后续云端连接

---

## 🔮 后期扩展

当需要支持云端AI对话时，可以：

1. 创建 `svc_backend_connection` 组件
2. 使用NVS中的 `deviceId` 和 `authCode`
3. 连接到 `wss://host/ws/device`（正式WebSocket）
4. 实现云端音频上传和AI回复接收
5. 通过menuconfig配置：本地模式 / 云端模式

---

## 📝 日志输出

### 未配对设备启动

```
I (123) app_main: ========== 设备启动 - 完整架构版本 ==========
I (456) app_main: ========== 设备未配对 ==========
I (456) app_main: 配对模式: 开启
I (456) app_main: 设备将启动配对流程，连接WiFi后访问设备IP地址查看配对码
I (789) app_main: Wi-Fi已连接
I (890) app_main: 获取IP地址: 192.168.1.100
I (891) app_main: Wi-Fi连接成功，启动配对流程
I (892) pairing_ui: 启动HTTP服务器，端口: 80
I (893) pairing_ui: 配对网页已启动，访问地址: http://192.168.1.100
I (894) backend_pairing: 配对服务初始化完成
I (895) backend_pairing: 开始配对流程，连接: wss://ai-server.com/ws/device/pair
I (1200) backend_pairing: WebSocket已连接
I (1201) backend_pairing: 发送hello消息
I (1300) backend_pairing: 配对码已生成: 628391 (有效期: 300秒)
```

### 已配对设备启动

```
I (123) app_main: ========== 设备启动 - 完整架构版本 ==========
I (456) app_main: ========== 设备已配对 ==========
I (456) app_main: 设备ID: device-uuid-xxx
I (456) app_main: 客户ID: tenant-uuid-yyy
I (456) app_main: 配对模式: 关闭，继续正常初始化
I (789) app_main: ========== 初始化完成，当前状态: BOOT ==========
```

---

## 🐛 调试技巧

1. **查看配对状态**: 通过串口监控日志输出
2. **访问状态API**: `http://<设备IP>/status` 查看JSON状态
3. **清除配对信息**: 调用 `backend_pairing_clear_nvs()` 重新配对
4. **查看NVS内容**: 使用 `nvs_flash` 工具查看存储内容

---

## 📞 问题排查

### 问题1: 无法连接配对服务器

**现象**: WebSocket连接失败

**排查**:
1. 检查后台服务器地址配置是否正确
2. 确认后台服务器正在运行
3. 检查WiFi网络连接是否正常
4. 确认防火墙没有阻止WebSocket连接

### 问题2: 网页无法访问

**现象**: 浏览器无法打开配对页面

**排查**:
1. 确认设备已连接WiFi并获取IP地址
2. 确认浏览器和设备在同一网络
3. 检查HTTP服务器是否成功启动（查看日志）
4. 尝试访问 `/status` 接口测试

### 问题3: 配对码不显示

**现象**: 网页空白或显示"等待中"

**排查**:
1. 检查WebSocket是否成功连接
2. 查看后台是否返回了 `pairing_code_ready` 消息
3. 确认回调函数 `app_on_pairing_event` 被正确调用
4. 检查日志中的配对码输出

---

## 📚 相关文档

- 后台服务器文档: `xn_ai_server/README.md`
- 设备协议文档: `xn_ai_server/docs/device-protocol.md`
- WiFi配网文档: `components/svc_wifi_provisioning/README.md`

---

**配对功能实现完成！** ✅
