# ESP32-P4 + ESP32-C5 网页配网方案

## 目标

本工程在 ESP32-P4 上运行业务和 TCP/IP 协议栈，通过 ESP-Hosted SDIO 控制 ESP32-C5 的 Wi-Fi。设备启动后固定进入 `AP+STA` 模式：

- SoftAP 始终可用，手机可随时连接热点维护网络配置。
- STA 在后台自动轮询已经保存的 Wi-Fi。
- 开机联网和断网重连均对每条已保存 Wi-Fi 最多尝试 3 次。
- 全部配置连接失败后进入配网状态，但热点和网页不会关闭。
- 网页支持扫描 Wi-Fi、保存并连接、查看已保存 Wi-Fi、删除已保存 Wi-Fi、查看当前连接 Wi-Fi、STA 地址和配网热点地址。

## 组件划分

### `components/wifi_provisioning`

独立配网组件，直接使用 ESP-IDF 标准 `esp_wifi_*` API。在本项目中这些 API 由 `esp_wifi_remote` 转发到 ESP32-C5，因此组件不包含业务代码。

- `wifi_provisioning.c`：AP+STA 初始化、Wi-Fi 事件、连接状态机、三次重试、已保存 Wi-Fi 轮询和断网恢复。
- `wifi_provisioning_storage.c`：使用 NVS 保存最多 8 条 Wi-Fi，列表首项为最高连接优先级；密码不会写入日志，也不会返回给网页。
- `wifi_provisioning_web.c`：内置离线单页网页和 HTTP API，不依赖公网资源。
- `include/wifi_provisioning.h`：提供给其他项目或业务层复用的公共接口。
- `Kconfig`：配置热点名称、热点密码、信道、客户端数量、HTTP 端口和 STA 最低认证方式。

### `main/p4_c5_wifi.c`

业务适配层。原有业务继续调用 `p4_c5_wifi_start()`、`p4_c5_wifi_wait_connected()`、`p4_c5_wifi_is_connected()` 和 `p4_c5_wifi_get_ip_info()`，不直接操作 Wi-Fi 驱动。

### `components/voice_board_audio`

ESP32-P4-WIFI6-DB 的最小板级音频组件，只引入 ES8311、I2C、I2S 和 GPIO
依赖，不引入显示、触摸、摄像头和 LVGL：

- I2C：SDA GPIO7、SCL GPIO8，控制器 1。
- I2S：MCLK GPIO13、BCLK GPIO12、WS GPIO10、DOUT GPIO9、DIN GPIO11，控制器 1。
- 功放使能：GPIO53；语音识别仅使用 ADC 输入，功放保持关闭。
- 采集格式：16 kHz、16 bit、单声道，麦克风默认增益 24 dB。

### `components/voice_service`

可复用的 ESP-SR MultiNet 语音组件：

- 加载 `model` 分区中的 `mn7_cn` 中文模型。
- 注册命令 ID 1 和拼音命令 `ni hao xiao fei`，对应“你好小飞”。
- 独立 FreeRTOS 任务持续读取 PCM 并调用 MultiNet 检测。
- 通过回调上报初始化完成、唤醒、识别窗口超时、音频错误、模型错误和停止事件。
- 业务回调只做队列投递，不直接执行网络请求、音频播放或其他阻塞操作。

当前业务入口在 `main/app_main.c` 注册回调并启动服务。识别到“你好小飞”后，
业务层收到 `VOICE_SERVICE_EVENT_WAKE_DETECTED`，后续 AI 对话状态机从该事件继续执行。

## 状态机

1. `STARTING`：初始化 NVS、ESP-Netif、默认事件循环、ESP-Hosted、AP/STA 网络接口、Wi-Fi 驱动和 HTTP 服务。
2. `CONNECTING`：按 NVS 列表顺序读取保存的 Wi-Fi；每条配置连续尝试 3 次，单次最长等待 DHCP 12 秒。
3. `CONNECTED`：STA 已取得 DHCP 地址；成功配置自动移动到保存列表首位。
4. `PROVISIONING`：没有保存配置或全部配置均连接失败；SoftAP 和网页继续工作。
5. 已联网状态发生断网时，事件处理器只投递一次重连命令，再次执行步骤 2；连接尝试过程中产生的断开事件只结束当前尝试，不会重复投递重连命令。

## 默认使用方式

1. 给 P4 和 C5 烧录匹配固件后启动设备。
2. 手机连接热点 `FengFeifei-AI`，默认密码 `12345678`。
3. 浏览器访问 `http://192.168.4.1`。
4. 点击“扫描附近 Wi-Fi”，选择目标网络，输入密码后点击“保存并连接”。
5. 页面每 3 秒刷新状态；显示“已联网”时会同时显示当前 SSID 和 STA 地址。

热点名称、密码和 HTTP 端口可通过 `idf.py menuconfig` 的“Wi-Fi 网页配网组件”菜单修改。

## HTTP API

| 方法 | 路径 | 功能 |
| --- | --- | --- |
| `GET` | `/` | 返回内置配网页面 |
| `GET` | `/api/status` | 返回状态、当前 SSID、STA/AP 地址和累计尝试次数 |
| `GET` | `/api/scan` | 扫描并返回最多 20 个附近 Wi-Fi |
| `GET` | `/api/saved` | 返回已保存 SSID，绝不返回密码 |
| `POST` | `/api/connect` | 接收 `ssid`、`password`，写入 NVS 并异步连接 |
| `POST` | `/api/delete` | 接收 `ssid` 并删除对应保存配置 |

`POST` 请求使用 `application/x-www-form-urlencoded`。

## 业务接入

业务启动时只初始化一次：

```c
ESP_ERROR_CHECK(p4_c5_wifi_start());
```

需要联网后才能启动的云业务，可在独立任务中等待：

```c
if (p4_c5_wifi_wait_connected(pdMS_TO_TICKS(30000)) == ESP_OK) {
    /* 在此启动必须依赖互联网的业务模块。 */
}
```

## 构建

构建脚本位于 `build/build.sh`，符合项目脚本目录约束。先加载 ESP-IDF 6.1 环境，再执行：

```bash
bash build/build.sh
```

脚本将构建产物放入 `build/esp-idf-output`。

Windows PowerShell 使用 `build/build.ps1`，它会固定 ESP-IDF 工具链路径并避免
RISC-V C++ 头文件的 Windows 长路径问题：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\build\build.ps1
```

需要清理构建产物时执行：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\build\build.ps1 -Clean
```
