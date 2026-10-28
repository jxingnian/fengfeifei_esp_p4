# 视频上行组件只依赖摄像头 BSP 和 JPEG 硬件驱动，不依赖屏幕和音频。
idf_component_register(
    SRCS "video_uplink.c"
    INCLUDE_DIRS "include"
    # 源码直接使用 esp_timer.h，因此必须声明对应的公共组件依赖。
    REQUIRES esp32_p4_wifi6_db esp_driver_jpeg esp_timer
)
