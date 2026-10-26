/*
 * 兼容 media_lib_sal 使用的 Mbed TLS 3.x SHA-256 接口。
 *
 * ESP-IDF 6.1 的 Mbed TLS 4 私有头仍提供 SHA-224/SHA-256 的真实上下文
 * 和非 _ret 函数。这里直接复用这些声明，只补充旧版第三方组件需要的
 * _ret 函数，保证上下文布局、内存大小和底层实现完全一致。
 */
#pragma once

/* 允许兼容头访问 IDF 仅供 Mbed TLS 内部使用的 SHA-256 声明。 */
#ifndef MBEDTLS_ALLOW_PRIVATE_ACCESS
#define MEDIA_LIB_MBEDTLS_ALLOW_PRIVATE_ACCESS
#define MBEDTLS_ALLOW_PRIVATE_ACCESS
#endif

#include "mbedtls/private/sha256.h"

/* 上述宏只服务于包含私有声明，避免污染包含本头文件的其他代码。 */
#ifdef MEDIA_LIB_MBEDTLS_ALLOW_PRIVATE_ACCESS
#undef MBEDTLS_ALLOW_PRIVATE_ACCESS
#undef MEDIA_LIB_MBEDTLS_ALLOW_PRIVATE_ACCESS
#endif

/* 将 Mbed TLS 3.x 的 _ret 名称映射到 IDF 6.1 保留的同等实现。 */
static inline int mbedtls_sha256_starts_ret(mbedtls_sha256_context *context,
                                            int is224)
{
    return mbedtls_sha256_starts(context, is224);
}

static inline int mbedtls_sha256_update_ret(mbedtls_sha256_context *context,
                                            const unsigned char *input,
                                            size_t length)
{
    return mbedtls_sha256_update(context, input, length);
}

static inline int mbedtls_sha256_finish_ret(mbedtls_sha256_context *context,
                                            unsigned char output[32])
{
    return mbedtls_sha256_finish(context, output);
}
