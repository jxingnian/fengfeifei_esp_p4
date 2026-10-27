# ESP32-P4 + ESP32-C5 老人陪伴设备固件

当前固件只实现设备基础框架和 Coze 直连 AI 语音对话。视频通话、后台网关、
人体存在传感器、摔倒检测、按键和小程序属于后续独立功能，不进入当前构建链路。

## 当前架构

```text
main/app_main.c
  -> wifi_provisioning       Wi-Fi 配网与联网状态
  -> audio_service           统一音频资源、PCM 读写、功放和音量
  -> coze_app                Coze 直连会话与 Coze 音频协议
  -> ai_dialog               AI 对话状态机、上下行队列和 Opus 上行编码
  -> wake_word               MultiNet 中文唤醒词和单一麦克风采集任务
  -> audio_driver            ES8311/I2S/GPIO 板级驱动
```

音频格式固定为 16 kHz、单声道、16 bit；上行使用 60 ms Opus，下行线路使用
20 ms Opus。Coze SDK 的 `audio_callback` 只完成 Base64 解码，应用层把 Opus
帧放入 200 KB PSRAM 环形缓冲区，再由独立任务实时解码为 PCM 后播放。ES8311
扬声器音量配置为 100，Coze 下行每秒最多 50 个 20 ms 音频帧。

## 目录职责

- `components/audio_driver`：只包含 ES8311、I2S、I2C 和功放硬件操作。
- `components/audio_service`：唯一音频业务边界，负责所有权和 PCM 读写。
- `components/coze_app`：Coze Bot、令牌、用户 ID 和实时会话参数。
- `components/ai_dialog`：待机唤醒后的对话状态机、队列和上行编码。
- `components/wake_word`：中文 MultiNet 唤醒词以及共享采集任务。
- `components/wifi_provisioning`：设备本地 AP+STA 配网。
- `managed_components/espressif__esp_coze`：ESP-IDF 托管的第三方 Coze 组件，不修改。

## 启动链路

`NVS -> Wi-Fi 配网 -> audio_service -> ai_dialog/Coze -> wake_word -> 待机监听`。

唤醒词识别到“你好小飞”后，采集任务切换为 AI 对话模式；Coze 返回的 PCM 进入
有界播放队列，播放 DMA 排空后恢复待机监听。

## 构建

用户可在本工程目录执行自己的 ESP-IDF 构建流程。Coze 令牌只应写入本地
`sdkconfig` 或 menuconfig，不要写入源码；`sdkconfig.defaults` 只保存非敏感默认值。

Coze API 资料保留在 `docs/coze api`。
