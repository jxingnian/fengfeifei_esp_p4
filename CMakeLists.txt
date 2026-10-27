# ESP-IDF 工程的标准 CMake 入口。
cmake_minimum_required(VERSION 3.22)

# ESP-SR 模型脚本在 Windows 下需要 UTF-8 环境，避免系统区域设置影响
# sdkconfig 和生成文件中的中文路径与配置值。
set_property(
    GLOBAL PROPERTY RULE_LAUNCH_CUSTOM
    "${CMAKE_COMMAND} -E env PYTHONUTF8=1"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(p4_c5_ai_device)
idf_build_set_property(MINIMAL_BUILD ON)
