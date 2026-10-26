# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.22)

# 为仍使用 Mbed TLS 3.x 哈希接口的第三方组件提供 ESP-IDF 6.1 PSA 兼容头。
include_directories(BEFORE "${CMAKE_CURRENT_LIST_DIR}/components/mbedtls_compat/include")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# "Trim" the build. Include the minimal set of components, main, and anything it depends on.
idf_build_set_property(MINIMAL_BUILD ON)
project(p4_c5_network)
