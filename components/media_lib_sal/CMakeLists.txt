# 本地固定版本的 media_lib_sal 组件。
#
# 该组件原本由组件管理器生成到 managed_components 目录；将源码纳入项目
# 后，删除生成目录、重新解析依赖或执行全量构建都不会丢失项目兼容修复。
file(GLOB MEDIA_LIB_SAL_SOURCES
     "${CMAKE_CURRENT_LIST_DIR}/*.c"
     "${CMAKE_CURRENT_LIST_DIR}/port/*.c"
     "${CMAKE_CURRENT_LIST_DIR}/mem_trace/*.c")

idf_component_register(
    SRCS ${MEDIA_LIB_SAL_SOURCES}
    INCLUDE_DIRS "include" "include/port"
    REQUIRES esp-tls mbedtls esp_netif
)
