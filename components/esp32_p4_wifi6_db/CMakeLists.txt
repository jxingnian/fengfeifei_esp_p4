if("${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}" VERSION_GREATER_EQUAL "5.3")
    set(REQ esp_driver_gpio esp_driver_i2s esp_driver_sdmmc esp_driver_i2c)
    set(PRIV_REQ "")
else()
    set(REQ driver)
    set(PRIV_REQ "")
endif()

if(${IDF_VERSION_MAJOR} LESS 6)
    list(APPEND PRIV_REQ usb)
endif()

idf_component_register(
    SRCS "esp32_p4_wifi6_db.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "priv_include"
    REQUIRES ${REQ} fatfs esp_video
    PRIV_REQUIRES ${PRIV_REQ} esp_lcd esp_hw_support spiffs esp_psram
)

# 视频上行不使用屏幕和 LVGL；关闭 BSP 的图形库头文件依赖。
target_compile_definitions(${COMPONENT_LIB} PUBLIC BSP_CONFIG_NO_GRAPHIC_LIB=1)
