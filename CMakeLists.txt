# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.22)

# ESP-SR 的模型打包脚本会读取 UTF-8 编码的 sdkconfig；Windows 默认代码页为 GBK。
# 给 Ninja 的自定义构建命令统一注入 UTF-8 环境，避免依赖终端或用户环境变量。
set_property(GLOBAL PROPERTY RULE_LAUNCH_CUSTOM "${CMAKE_COMMAND} -E env PYTHONUTF8=1")

# 为仍使用 Mbed TLS 3.x 哈希接口的第三方组件提供 ESP-IDF 6.1 PSA 兼容头。
include_directories(BEFORE "${CMAKE_CURRENT_LIST_DIR}/components/mbedtls_compat/include")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# "Trim" the build. Include the minimal set of components, main, and anything it depends on.
idf_build_set_property(MINIMAL_BUILD ON)
project(p4_c5_network)
