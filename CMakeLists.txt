# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.22)

# ESP-SR 的模型打包脚本会读取 UTF-8 编码的 sdkconfig；Windows 默认代码页为 GBK。
# 给 Ninja 的自定义构建命令统一注入 UTF-8 环境，避免依赖终端或用户环境变量。
set_property(GLOBAL PROPERTY RULE_LAUNCH_CUSTOM "${CMAKE_COMMAND} -E env PYTHONUTF8=1")

# 为仍使用 Mbed TLS 3.x 哈希接口的第三方组件提供 ESP-IDF 6.1 PSA 兼容头。
include_directories(BEFORE "${CMAKE_CURRENT_LIST_DIR}/components/mbedtls_compat/include")

# ESP-IDF 6.1 beta 的 Windows picolibc 工具链会把 C++ 多架构目录加入系统头
# 文件搜索路径，但部分版本没有让 bits/cpu_defines.h 从 c++config.h 所在目录
# 正确解析。这里从当前 C++ 编译器位置自动寻找对应的 ilp32f 目录并提前加入
# 项目头文件路径，避免写死任何电脑用户名、磁盘盘符或工具链安装路径。
find_program(_esp_cxx_compiler NAMES riscv32-esp-elf-g++.exe riscv32-esp-elf-g++)
if(_esp_cxx_compiler)
    get_filename_component(_compiler_bin_dir "${_esp_cxx_compiler}" DIRECTORY)
    get_filename_component(_toolchain_root "${_compiler_bin_dir}" DIRECTORY)
    file(GLOB _picolibc_cxx_arch_dirs
         "${_toolchain_root}/picolibc/riscv32-esp-elf/include/c++/*/riscv32-esp-elf/rv32imafc*/ilp32f")
    foreach(_picolibc_cxx_arch_dir IN LISTS _picolibc_cxx_arch_dirs)
        if(EXISTS "${_picolibc_cxx_arch_dir}/bits/cpu_defines.h")
            include_directories(BEFORE "${_picolibc_cxx_arch_dir}")
            message(STATUS "Added picolibc C++ compatibility include: ${_picolibc_cxx_arch_dir}")
            break()
        endif()
    endforeach()
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# "Trim" the build. Include the minimal set of components, main, and anything it depends on.
idf_build_set_property(MINIMAL_BUILD ON)
project(p4_c5_network)
