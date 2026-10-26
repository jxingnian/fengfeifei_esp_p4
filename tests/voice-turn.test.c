#include "../components/voice_ai_service/voice_turn.h"
#define assert(condition) do { if (!(condition)) return __LINE__; } while (0)
static voice_turn_event_t feed(voice_turn_t *t, bool speech)
{
    return voice_turn_feed(t, speech, 60, 800, 5000, 15000);
}
int main(void)
{
    voice_turn_t t = {0};
    for (int i = 0; i < 83; ++i) assert(feed(&t, false) == TURN_LISTEN);
    assert(feed(&t, false) == TURN_EMPTY); // no speech: bounded exit
    t = (voice_turn_t){0};
    for (int i = 0; i < 20; ++i) { // isolated noise never starts speech
        assert(feed(&t, true) == TURN_LISTEN); assert(feed(&t, false) == TURN_LISTEN);
    }
    assert(feed(&t, true) == TURN_LISTEN); assert(feed(&t, true) == TURN_STARTED);
    for (int i = 0; i < 12; ++i) assert(feed(&t, false) == TURN_LISTEN);
    assert(feed(&t, true) == TURN_LISTEN); // short pause resets tail
    for (int i = 0; i < 13; ++i) assert(feed(&t, false) == TURN_LISTEN);
    assert(feed(&t, false) == TURN_COMMIT); assert(t.silence_ms == 840);
    t = (voice_turn_t){0};
    assert(feed(&t, true) == TURN_LISTEN); assert(feed(&t, true) == TURN_STARTED);
    for (int i = 2; i < 249; ++i) assert(feed(&t, true) == TURN_LISTEN);
    assert(feed(&t, true) == TURN_COMMIT); assert(t.elapsed_ms == 15000);
    // Every subsequent turn starts with a fresh policy state.
    t = (voice_turn_t){0}; assert(feed(&t, false) == TURN_LISTEN);
    assert(!t.started && t.silence_ms == 0);
    return 0;
}
