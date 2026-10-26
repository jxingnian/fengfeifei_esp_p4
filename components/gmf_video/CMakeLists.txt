# 本地固定版本的 GMF Video 组件。
#
# ESP-IDF 6.1 将 DMA2D 私有头文件放在 esp_driver_dma 组件中；显式声明该
# 依赖后，gmf_video_ppa.c 才能在全量构建时获得 esp_private/dma2d.h 的包含路径。
set(COMPONENT_REQUIRES)
if (${IDF_TARGET} STREQUAL "esp32p4")
    list(APPEND COMPONENT_REQUIRES esp_driver_ppa esp_driver_dma esp_hw_support esp_mm)
endif()

idf_component_register(
    SRC_DIRS "."
    INCLUDE_DIRS "./include"
    REQUIRES ${COMPONENT_REQUIRES}
    PRIV_INCLUDE_DIRS "./private_include"
)
