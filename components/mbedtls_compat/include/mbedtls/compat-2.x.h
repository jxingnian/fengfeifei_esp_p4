/*
 * 为仍然包含 Mbed TLS 2.x 兼容头的第三方组件提供空兼容入口。
 *
 * ESP-IDF 6.1 已删除原版 compat-2.x.h；media_lib_sal 只需要该文件能够
 * 被安全包含，不依赖其中的旧版宏定义，因此这里不重新引入已经失效的
 * 兼容宏，避免把 Mbed TLS 4 的新接口错误改写成旧接口。
 */
#ifndef MEDIA_LIB_MBEDTLS_COMPAT_2_X_H
#define MEDIA_LIB_MBEDTLS_COMPAT_2_X_H

/* 当前组件不需要 Mbed TLS 2.x 的额外宏，保留空头文件语义。 */

#endif /* MEDIA_LIB_MBEDTLS_COMPAT_2_X_H */
