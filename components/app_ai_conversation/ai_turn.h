#pragma once
#include <stdbool.h>
#include <stdint.h>

/* 纯采样时间策略，不依赖网络、RTOS 或音频编解码器。 */
typedef struct {
    uint32_t elapsed_ms, speech_ms, silence_ms;
    bool started;
} ai_turn_t;
typedef enum { AI_TURN_LISTEN, AI_TURN_STARTED, AI_TURN_COMMIT, AI_TURN_EMPTY } ai_turn_event_t;
static inline ai_turn_event_t ai_turn_feed(ai_turn_t *t, bool speech,
        uint32_t ms, uint32_t tail, uint32_t empty, uint32_t maximum)
{
    t->elapsed_ms += ms;
    if (!t->started) {
        t->speech_ms = speech ? t->speech_ms + ms : 0;
        if (t->speech_ms >= 120) { t->started = true; return AI_TURN_STARTED; }
        return t->elapsed_ms >= empty ? AI_TURN_EMPTY : AI_TURN_LISTEN;
    }
    t->silence_ms = speech ? 0 : t->silence_ms + ms;
    return t->silence_ms >= tail || t->elapsed_ms >= maximum ? AI_TURN_COMMIT : AI_TURN_LISTEN;
}
