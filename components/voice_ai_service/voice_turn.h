#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Pure sample-time policy; no network, RTOS or codec dependency. */
typedef struct {
    uint32_t elapsed_ms, speech_ms, silence_ms;
    bool started;
} voice_turn_t;
typedef enum { TURN_LISTEN, TURN_STARTED, TURN_COMMIT, TURN_EMPTY } voice_turn_event_t;
static inline voice_turn_event_t voice_turn_feed(voice_turn_t *t, bool speech,
        uint32_t ms, uint32_t tail, uint32_t empty, uint32_t maximum)
{
    t->elapsed_ms += ms;
    if (!t->started) {
        t->speech_ms = speech ? t->speech_ms + ms : 0;
        if (t->speech_ms >= 120) { t->started = true; return TURN_STARTED; }
        return t->elapsed_ms >= empty ? TURN_EMPTY : TURN_LISTEN;
    }
    t->silence_ms = speech ? 0 : t->silence_ms + ms;
    return t->silence_ms >= tail || t->elapsed_ms >= maximum ? TURN_COMMIT : TURN_LISTEN;
}
