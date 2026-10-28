# ESP-IDF 6.1-beta1 与组件管理器组件的兼容修复。
#
# 组件管理器生成的 managed_components 目录不纳入版本控制，因此不能把兼容修改
# 只写在该目录中。本文件在 IDF 配置前对已解析的组件源码做幂等修复，确保组件
# 被重新下载或重新生成后，工程仍然使用与当前 IDF 驱动匹配的编译条件。

set(_esp_video_caps_file
    "${CMAKE_SOURCE_DIR}/managed_components/espressif__esp_video/include/esp_video_caps.h")
if(EXISTS "${_esp_video_caps_file}")
    file(READ "${_esp_video_caps_file}" _esp_video_caps_content)

    # IDF 6.1-beta1 删除 CSI 错误事件回调，禁用 esp_video 2.5 的旧事件分支。
    string(REPLACE
        [=[(ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 5, 6) && ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(6, 0, 0)) || \
    ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 3)]=]
        [=[(ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 5, 6) && ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(6, 0, 0)) || \
    (ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 3) && ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(6, 1, 0))]=]
        _esp_video_caps_content "${_esp_video_caps_content}")

    file(WRITE "${_esp_video_caps_file}" "${_esp_video_caps_content}")
endif()

set(_esp_lvgl_adapter_cmake
    "${CMAKE_SOURCE_DIR}/managed_components/espressif__esp_lvgl_adapter/CMakeLists.txt")
if(EXISTS "${_esp_lvgl_adapter_cmake}")
    file(READ "${_esp_lvgl_adapter_cmake}" _esp_lvgl_adapter_content)

    # LVGL 9.6 的旧兼容头会发出弃用预处理警告；仅对适配器组件关闭该警告升级，
    # 保留工程其它组件的 -Werror 检查，避免隐藏真正的编译错误。
    # 删除历史版本兼容层写入的编译选项。组件管理器会在脚本模式下读取该文件，
    # 此阶段不能执行 target_compile_options；真正的编译选项在顶层目标创建后追加。
    string(REPLACE
        "target_compile_options(\${COMPONENT_LIB} PRIVATE -Wno-error=cpp)\n"
        ""
        _esp_lvgl_adapter_content "${_esp_lvgl_adapter_content}")
    file(WRITE "${_esp_lvgl_adapter_cmake}" "${_esp_lvgl_adapter_content}")
endif()

# LVGL 9.6 的旧兼容头会发出弃用预处理警告；仅对适配器组件关闭该警告升级，
# 保留工程其它组件的 -Werror 检查，避免隐藏真正的编译问题。
idf_component_get_property(_esp_lvgl_adapter_lib
    espressif__esp_lvgl_adapter COMPONENT_LIB)
if(TARGET "${_esp_lvgl_adapter_lib}")
    target_compile_options("${_esp_lvgl_adapter_lib}" PRIVATE -Wno-error=cpp)
endif()
