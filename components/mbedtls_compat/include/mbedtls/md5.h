/*
 * 兼容 media_lib_sal 使用的 Mbed TLS 3.x MD5 接口。
 *
 * ESP-IDF 6.1 搭载 Mbed TLS 4，公共 mbedtls/md5.h 已经移除，但其内置
 * 实现仍保留了旧的 MD5 上下文和非 _ret 函数。这里复用 IDF 的真实类型
 * 和函数，只补充 Mbed TLS 3.x 代码仍会调用的三个 _ret 函数，避免重新
 * 定义上下文而破坏 ABI 或与 Mbed TLS 内部实现发生类型冲突。
 */
#pragma once

/* 允许兼容头访问 IDF 仅供 Mbed TLS 内部使用的 MD5 声明。 */
#ifndef MBEDTLS_ALLOW_PRIVATE_ACCESS
#define MEDIA_LIB_MBEDTLS_ALLOW_PRIVATE_ACCESS
#define MBEDTLS_ALLOW_PRIVATE_ACCESS
#endif

#include "mbedtls/private/md5.h"

/* 上述宏只服务于包含私有声明，避免污染包含本头文件的其他代码。 */
#ifdef MEDIA_LIB_MBEDTLS_ALLOW_PRIVATE_ACCESS
#undef MBEDTLS_ALLOW_PRIVATE_ACCESS
#undef MEDIA_LIB_MBEDTLS_ALLOW_PRIVATE_ACCESS
#endif

/* 将 Mbed TLS 3.x 的 _ret 名称映射到 IDF 6.1 保留的同等实现。 */
static inline int mbedtls_md5_starts_ret(mbedtls_md5_context *context)
{
    return mbedtls_md5_starts(context);
}

static inline int mbedtls_md5_update_ret(mbedtls_md5_context *context,
                                         const unsigned char *input,
                                         size_t length)
{
    return mbedtls_md5_update(context, input, length);
}

static inline int mbedtls_md5_finish_ret(mbedtls_md5_context *context,
                                         unsigned char output[16])
{
    return mbedtls_md5_finish(context, output);
}
