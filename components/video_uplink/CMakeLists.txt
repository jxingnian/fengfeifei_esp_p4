# 视频上行组件只依赖摄像头 BSP 和 JPEG 硬件驱动，不依赖屏幕和音频。
idf_component_register(
    SRCS "video_uplink.c"
    INCLUDE_DIRS "include"
    REQUIRES esp32_p4_wifi6_db esp_driver_jpeg
)
