# 设备后台接入

## 使用流程

1. 手机连接设备热点，在 `http://192.168.4.1` 配置 Wi-Fi。
2. 同一页面的“后台连接与设备配对”区域填写 HTTPS 后台源地址（默认 `https://iot.xingnian.vip`，不带接口路径）。客户 ID 选填：填写后台组织 ID 后该客户管理员可查看；留空时只有平台管理员能看到待配对设备。
3. 点击“连接后台 / 重新连接”，设备会自动获得 6 位授权码，并在本地状态页面显示。后台设备管理按设备 UID 查找待配对设备，管理员或小程序用户输入该授权码。
4. 后台选择客户，可选指定后台用户；小程序自动绑定当前小程序用户。服务端返回长期授权码，设备将其与设备 ID、服务器地址一起提交到 NVS，再建立正式连接。
5. 重启后自动使用保存的授权连接。授权码有效期 5 分钟且成功后立即失效；临时连接最多 10 分钟，断开后需重新连接并使用新授权码。

`/cloud` 也可单独打开。默认仅允许 HTTPS，证书链和域名通过 ESP-IDF 证书包校验；联网后通过 NTP 同步系统时间。局域网调试可在 menuconfig 开启 `CLOUD_GATEWAY_ALLOW_INSECURE` 后使用 HTTP（授权将明文传输）。已有 sdkconfig 的工程应确认 `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y`。

## 组件边界

- `wifi_provisioning` 负责 AP/STA、网络轮询和共享 HTTP 服务；启动前注册静态扩展路由。
- `cloud_gateway.c` 的唯一工作任务负责 WebSocket 生命周期、消息队列、配对状态机、自动重连和认证。网络回调仅收集有界消息并投递队列；连接代次隔离旧连接遗留消息。
- `cloud_gateway_storage.c` 在 NVS 的 `cloud_gateway/identity` 保存一个版本化 blob，服务器地址、客户 ID、设备 ID、授权码同时提交。页面、状态 API 和日志不返回授权码。NVS 当前不是加密存储；量产若需要抵御物理读取，应配置 Flash/NVS 加密。
- `cloud_gateway_web.c` 提供独立页面和本地 API，配网页面嵌入该页面。所有代码不依赖语音/摄像头组件。
- `main/app_main.c` 注册 Web 路由，启动 Wi-Fi，再启动网关；语音唤醒服务继续独立运行。

## 本地 API

GET `/api/cloud/status` 返回 state、paired、connected、serverUrl、tenantId、deviceUid、deviceId、pairingId、pairingCode、message、retrySeconds。`pairingCode` 仅在设备本地页面返回；长期授权码不返回。connected 仅代表 WebSocket 已打开；state=6 才代表正式连接 ready。

POST 请求必须同时带 `Content-Type: application/json`、`X-Cloud-Control: 1`。不开放跨域访问。202 仅表示命令入队，最终结果通过状态接口查询。

| 接口 | JSON 请求 |
| --- | --- |
| `/api/cloud/configure` | `{"serverUrl":"https://iot.xingnian.vip","tenantId":"组织ID或空字符串"}` |
| `/api/cloud/forget` | `{}` |

配对后禁止直接更改服务器或客户，需先解除本地授权；相同配置可用于重新连接。解除本地授权不撤销后台权限，管理员还应撤销旧设备记录。管理接口沿用现有配网的局域网信任边界，没有独立登录；仅供可信热点/局域网使用，不能映射到公网。

## 后台协议与异常恢复

临时连接为 `/ws/device/pair`：设备发送 hello（稳定 eFuse MAC 派生 UID、固件版本、麦克风/扬声器能力、可选 tenantId），收到包含 `code` 的 `pairing_pending` 后显示授权码。后台或小程序提交授权码，服务端在同一连接返回 `paired/deviceId/deviceAuthCode`；设备持久化后再连接 `/ws/device`。

- 普通断线采用带抖动的指数退避，最长约 60 秒；Wi-Fi 断开时等待联网。
- AUTH_REVOKED 清除已保存授权并重新进入配对；持久化失败则停止重连，等待人工清除。
- AUTH_FAILED 保留授权并停止重连；修复后台客户/设备状态后点击重新连接。
- 授权码过期或临时连接断开后，重新连接设备并使用新显示的授权码。
- 授权保存失败时停止重连，避免未保存的身份被当成已成功配对。需管理员撤销本次后台记录，再重新配对。
- 等待同步时间时检查 NTP/DNS；TLS 或网络握手失败时检查服务器证书、端口和网络。

## 后续语音和摄像头

调用 `cloud_gateway_start()` 时可注册 on_message 回调；回调在网关任务运行，数据仅在回调期间有效，业务应复制并投递自己的任务，不能阻塞或执行媒体处理。`cloud_gateway_send(binary, data, length)` 非阻塞入队，仅 ready 后允许发送，单条上限 16 KiB、共享队列 12 条，队列满返回错误。接收支持 WebSocket 分片重组，超过上限或队列溢出会断线重连，业务层必须处理背压。

本次只实现接入和授权基础，不包含采集上传、语音播放、摄像头驱动、音视频编码和通话状态机。当前标准语音后端整段返回 MP3 可能超过 16 KiB，接入播放前需要制定分块协议；不能直接把摄像头大帧塞入此队列。后续建议业务组件分为语音会话、摄像头采集、通话信令和媒体传输；后台新增通话会话/媒体授权协议后，再接入专门的实时媒体通道。已有后台设备协议见 `../../xn_ai_server/docs/device-protocol.md`。

## 验证清单

在具备 ESP-IDF 6.x 和 ESP32-P4 工具链的机器运行 `powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\build\build.ps1`（可通过 IDF_PATH、IDF_TOOLS_PATH 指定安装位置），首次构建解析 esp_websocket_client 组件依赖。构建成功后上板验证：首次配对、授权码过期、重启免配对、Wi-Fi 断线恢复、后台重启、AUTH_FAILED 保留授权、后台撤销后重新配对、更换服务器前清除本地授权。
