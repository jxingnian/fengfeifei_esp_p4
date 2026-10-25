# RTC 媒体组件兼容适配

LiveKit ESP32 SDK 固定 `livekit/livekit 0.3.11`（developer preview）。其注册表依赖
`esp_capture ~0.7` 与 `av_render ~0.9.1`，默认要求旧版 ESP-SR、esp_video、esp_codec_dev，
与本项目 ESP-SR 2.6、厂家 esp_video 2.3、esp_codec_dev 1.5 冲突。
因此本地覆盖以下组件；没有修改原有唤醒模型或降低厂家摄像头驱动版本。

| 本地组件 | 来源 | 下载归档 SHA256 |
| --- | --- | --- |
| components/esp_capture | espressif/esp_capture 0.7.11 | e70f1a9f84cb387e3ed0da090e9e57ee4d67efefac21dbca10ebcc190d629ddf |
| components/av_render | tempotian/av_render 0.9.1 | 1057d2c366973903830875ab0d2408cbfe7e989918c0c8c79190bb58fcff7e2e |

来源为 components.espressif.com 官方组件注册表。各源文件保留原许可声明，
esp_capture 保留原 LICENSE。只导入本板实际使用的源代码，不导入示例、测试素材或二进制库。

适配内容：

- capture 仅编译 ES8311 普通音频源、P4 V4L2 视频源、公共 GMF 管线；不编译旧 AEC/ESP-SR 源、文件源、字符叠加或字体。
- capture 的 esp_video 依赖调整为 ^2.3.0；不再依赖旧 ESP-SR 2.1.5。
- render 的 esp_codec_dev 依赖调整为 ~1.5；使用现代组件注册，不编译 LCD 实现，不依赖显示颜色转换的预编译归档。
- render 的 av_render_set_fixed_frame_info 原版成功保存格式却返回 WRONG_STATE，修正为 OK。
- capture 将管线失败回传应用，视频启动失败不再被音频启动的成功结果覆盖；V4L2 检查 STREAMON、使用 -1 表示无效 fd 并复位停止状态。render 正确回传 I2S 写失败。
- capture 增加首个有效音频/视频帧事件，设备只在实际产生两种媒体帧后接听；V4L2 检查缓冲区数量、映射及排队错误，避免初始化失败仍宣称成功。
- ES8311 源使用 64 位累计采样数计算时间戳，修复原版 32 位乘法在约两分钟通话后溢出的问题。
- 视频解码/显示接口保留链接兼容性，但应用只注册音频解码器，LiveKit 只订阅音频，不创建 LCD。

升级 LiveKit 或媒体组件时应逐项复核这些差异，并执行目标 ESP-IDF 的 reconfigure/build 和真机音视频回归。
本地源代码适配不等于已通过真机编译或声学验证；尤其尚无设备端 AEC，免提回声需后续实机调优。
