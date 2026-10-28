/**
 * @file voice_call.h
 * @brief 语音通话业务组件
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 通话事件类型
 */
typedef enum {
    VOICE_CALL_EVENT_INCOMING = 0,      // 来电
    VOICE_CALL_EVENT_CONNECTED,         // 通话接通
    VOICE_CALL_EVENT_DISCONNECTED,      // 通话断开
    VOICE_CALL_EVENT_ERROR,             // 错误
} voice_call_event_type_t;

typedef struct {
    voice_call_event_type_t type;
    const char *caller_id;              // 来电者ID
    const char *message;                // 错误消息
} voice_call_event_t;

typedef void (*voice_call_event_callback_t)(const voice_call_event_t *event, void *context);

typedef struct {
    voice_call_event_callback_t on_event;
    void *context;
} voice_call_config_t;

/**
 * @brief 初始化语音通话服务
 */
esp_err_t voice_call_init(const voice_call_config_t *config);

/**
 * @brief 反初始化语音通话服务
 */
void voice_call_deinit(void);

/**
 * @brief 播放来电铃声
 */
esp_err_t voice_call_ring(const char *caller_id);

/**
 * @brief 接听通话
 */
esp_err_t voice_call_accept(void);

/**
 * @brief 拒接通话
 */
esp_err_t voice_call_reject(void);

/**
 * @brief 挂断通话
 */
esp_err_t voice_call_hangup(void);

/**
 * @brief 检查是否正在通话
 */
bool voice_call_is_active(void);

#ifdef __cplusplus
}
#endif
