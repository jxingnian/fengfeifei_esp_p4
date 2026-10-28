# video_uplink

`video_uplink` 是 ESP32-P4 视频通话业务使用的本地视频上行组件。

## 组件职责

- 使用厂家 BSP 初始化 MIPI-CSI 摄像头；
- 使用 PSRAM 分配 V4L2 用户态采集缓冲；
- 将摄像头输出配置为 RGB565 或 RGB888；
- 使用 ESP32-P4 JPEG 硬件编码器生成 JPEG 帧；
- 通过 JPEG 回调把帧交给上层网络业务；
- 提供采集帧数、编码帧数、丢帧数和编码耗时统计。

组件不包含 LCD、视频解码、音频、网络连接和通话信令。

## 使用方式

```c
static void on_jpeg(const video_uplink_jpeg_frame_t *frame, void *user_ctx)
{
    /* 这里把 frame->data/frame->length 交给视频通话网络层发送。 */
    (void)frame;
    (void)user_ctx;
}

void start_video(void)
{
    video_uplink_config_t config;
    video_uplink_config_default(&config);
    config.jpeg_frame_cb = on_jpeg;
    ESP_ERROR_CHECK(video_uplink_init(&config));
    ESP_ERROR_CHECK(video_uplink_start());
}
```

JPEG 回调只在当前帧处理期间有效，网络层必须在回调中完成发送或复制数据，
不能在回调返回后继续保存 `frame->data` 指针。
