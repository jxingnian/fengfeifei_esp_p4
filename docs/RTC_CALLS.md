# ESP32-P4-WIFI6-DB 无屏音视频通话

## 板级适配

依据同目录厂家工程 `ESP32-P4-WIFI6-DB` 的 BSP 和 board manager 默认配置：

| 资源 | 本工程使用方式 |
| --- | --- |
| I2C1 GPIO7 SDA / GPIO8 SCL | ES8311、摄像头 SCCB 共用已创建总线，不重复创建或销毁 |
| I2S1 MCLK13 / BCLK12 / WS10 / DOUT9 / DIN11 | ES8311 16 kHz、16 bit、单声道录放音 |
| GPIO53 | 通话时功放开启，退出后关闭 |
| MIPI CSI + LDO3 2.5 V | esp_video 管理电源，reset/pwdn 为 -1，不初始化 LCD |
| 摄像头 | 默认 OV5647 RAW8 800×1280@50 传感器模式，H.264 发布目标 15 fps |
| P4 + C5 Hosted SDIO | 保留原工程网络引脚、16 MB Flash、PSRAM 200 MHz 和唤醒模型 |

不要整份覆盖厂家 sdkconfig：厂家示例还包含 32 MB Flash、LCD 等本项目不需要的设置。
如果实物摄像头是 SC2336 或其他型号，需在 menuconfig 选择对应传感器及模式，并同步
`CONFIG_CALL_VIDEO_WIDTH/HEIGHT`；不能只改网页分辨率。实际输出帧率和带宽须上板测量。

## 代码入口与生命周期

- `components/call_service/call_service.c`：业务 WS 非阻塞投递到有界队列，由单一 worker 管理呼叫。
- `components/call_service/call_media.c`：厂商注册与中立媒体接口，worker 不引用厂商 SDK 类型。
- `components/call_service/call_media_provider.h`：媒体适配 ops，支持后续并行注册厂商。
- `components/call_service/call_media_livekit.c`：共享板级音频句柄、V4L2 采集、Opus/H.264、LiveKit 房间、音频播放。
- `components/voice_board_audio`：借用总线/codec 接口，通话结束恢复识别采样格式及增益。
- `main/app_main.c`：先初始化语音和通话队列、探测摄像头，再启动业务网关。
- `components/esp_capture`、`components/av_render`：固定版本的本地兼容组件，来源与修改见 `RTC_THIRD_PARTY.md`。

设备发布麦克风与摄像头，只订阅远端语音，不解码远端视频，不需要显示屏。
收到 offer 后先安全停止 MultiNet 读取，连接媒体房间，**音频和视频都产生首帧**才发送接听。
失败、忙线、过期、媒体断开、业务网关断线会拒接/挂断，关闭媒体后恢复语音识别。
识别任务停止超时不强删任务、不抢占正在读的 codec，而是拒接并由 worker 延迟重试恢复。
原有裸 PCM 通话仍保留兼容。

摄像头启动探测失败时不上报 camera/webrtc，不影响原有语音监听和配网；
检查接线、型号和日志后重启，使能力重新上报。后端收到鉴权能力更新即保存，旧配对无需重配。

## 构建与烧录

本项目使用 ESP-IDF 6.1 系列，当前脚本默认 `D:\esp\v6.1-beta1\esp-idf`，
可通过 `IDF_PATH` 覆盖。还须安装相应 P4 RISC-V 工具链并配置短路径 `IDF_TOOLS_PATH`。
LiveKit 固定 `0.3.11`，其 esp_capture/av_render 通过组件清单覆盖成本地适配版；
首次构建需联网下载新增组件。不要删除本地媒体组件，否则会回到旧 ESP-SR/codec 依赖冲突。

PowerShell（把路径改成真实安装位置）：

```powershell
$env:IDF_PATH = 'D:\esp\v6.1-beta1\esp-idf'
$env:IDF_TOOLS_PATH = 'C:\EspressifTools'
. "$env:IDF_PATH\export.ps1"
idf.py -B build/esp-idf-output reconfigure
idf.py -B build/esp-idf-output menuconfig
idf.py -B build/esp-idf-output build
idf.py -B build/esp-idf-output -p COM7 flash monitor
```

也可在环境配置好后运行 `powershell -ExecutionPolicy Bypass -File build/build.ps1`。
`sdkconfig.defaults` 仅为默认值，已有 sdkconfig 不会自动采用新增默认项：
在 menuconfig 核对摄像头模式、Call 菜单、PSRAM、证书包、DTLS/SRTP 和 WS 动态缓冲配置。
不要用清空 sdkconfig 的方式丢失已有硬件配置。
组件管理器会更新 `dependencies.lock`，应在真正 reconfigure 后审查锁文件，不手填依赖哈希。
先确认新固件及唤醒模型仍适合现有分区；不需擦除整个 Flash 或配对 NVS。

设备配网和配对流程如下：设备联网后保存后台 HTTPS 地址，并主动连接后台登记为待配对设备；后台生成 6 位配对码，后台页面和小程序页面只显示这一个后台生成的配对码。用户打开设备本地网页，在设备网页输入配对码并提交；后台按配对码创建时指定的客户及可选后台用户绑定设备。小程序不创建、不提交配对码，设备也不生成配对码。
P4 中不存 LiveKit API secret，通话期间临时令牌由已鉴权业务连接下发。
HTTPS/WSS 需要时间同步与正确证书；沿用网关现有 SNTP，不关闭证书校验。

后台部署与网页测试详见 `../../xn_ai_server/docs/RTC_CALLS.md`。
微信小程序复用相同固件和 LiveKit 设备发布轨道，网关方案见 `../../xn_ai_server/docs/MINI_RTC.md`。

## 厂商扩展

设备成功探测摄像头时额外上报 `rtc-livekit`。offer 的 `provider` 决定媒体适配器，
历史无 provider 的 WebRTC offer 默认 LiveKit；不支持的厂商返回 `unsupported_provider`。
协议 worker 只依赖中立的 CONNECTING/CONNECTED/FAILED/DISCONNECTED 状态。
接入声网需新增并注册 ops、初始化与能力探测、CMake 依赖和 `rtc-agora` 能力声明，
由适配器解析对应凭证与 SDK 资源，不把厂商类型带回通话业务。
当前只实现 LiveKit；不能仅修改后台 provider 就让现有固件接入声网。

## 必须完成的真机验收

1. 检查启动识别正常、CSI sensor 初始化成功、后台能力包含 microphone/speaker/camera/webrtc。
2. 网页“测试设备 → 真实设备 → 音视频通话测试 → 呼叫设备”，应在 35 秒内接听。
3. 镜头移动，确认网页显示实时画面；对设备说话，网页可听见；对网页麦克风说话，设备扬声器可听见。
4. 检查视频方向、色彩、帧率、端到端延迟、Opus 声音清晰度和功放音量；根据实物调整 CALL 菜单。
5. 静音/取消静音；连续呼叫挂断至少 10 次，检查每次首帧、堆内存和句柄是否恢复。
6. 通话至少 10 分钟，检查音频时间戳连续、音画同步、PSRAM/内部堆余量、温升和网络带宽。
7. 挂断后应关闭功放、恢复“你好小飞”监听；网页关闭、设备 Wi-Fi 断开、后台断开时也应释放媒体。
8. 两个浏览器同时呼叫仅一个成功；跨客户不可呼叫；撤销配对/停用客户会断开。
9. 拔摄像头或用错误型号模式时不得假接通；恢复接线后重启再测。
10. 分别测试同网和公网/移动网络；信令通但无媒体时检查 LiveKit ICE、端口与 TURN。

本地已执行网页/后端测试及设备内嵌配网页面的宿主测试；**本机未安装 ESP-IDF，未完成固件编译或上述真机验收**。
LiveKit ESP32 SDK 处于 developer preview，发布前必须完成本板硬件验证。
当前没有设备端 AEC（单声道 ES8311 录音未加入扬声器参考信号），免提双讲回声需后续声学调优；
首次验证请将网页耳机与设备分开，不能把浏览器 AEC 当作设备回声消除已实现。
