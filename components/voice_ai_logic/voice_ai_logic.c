/**
 * @file voice_ai_logic.c
 * @brief AI 对话状态机实现。
 */

#include "voice_ai_logic.h"

#include <stdatomic.h>

/*
 * 状态只由本文件维护。应用层的编码任务、网络回调和播放任务通过原子读
 * 取得状态，所有状态迁移统一经过 voice_ai_logic_handle_event()。
 */
static atomic_int s_state = VOICE_AI_LOGIC_STATE_IDLE;
static atomic_bool s_initialized = false;

esp_err_t voice_ai_logic_init(void)
{
    atomic_store(&s_state, VOICE_AI_LOGIC_STATE_IDLE);
    atomic_store(&s_initialized, true);
    return ESP_OK;
}

static bool event_is_reset(voice_ai_logic_event_t event)
{
    return event == VOICE_AI_LOGIC_EVENT_BACKEND_ERROR ||
           event == VOICE_AI_LOGIC_EVENT_BACKEND_STOPPED ||
           event == VOICE_AI_LOGIC_EVENT_CALL_PREEMPT ||
           event == VOICE_AI_LOGIC_EVENT_STOP;
}

esp_err_t voice_ai_logic_handle_event(voice_ai_logic_event_t event)
{
    if (!atomic_load(&s_initialized)) {
        return ESP_ERR_INVALID_STATE;
    }

    voice_ai_logic_state_t current = (voice_ai_logic_state_t)atomic_load(&s_state);
    if (event_is_reset(event)) {
        atomic_store(&s_state, VOICE_AI_LOGIC_STATE_IDLE);
        return ESP_OK;
    }

    switch (event) {
    case VOICE_AI_LOGIC_EVENT_WAKE_DETECTED:
        /* 只有待机状态接受新的唤醒，避免重复唤醒创建并行会话。 */
        if (current == VOICE_AI_LOGIC_STATE_IDLE) {
            atomic_store(&s_state, VOICE_AI_LOGIC_STATE_STARTING);
        }
        break;

    case VOICE_AI_LOGIC_EVENT_BACKEND_READY:
        /* 后端就绪只对本次唤醒有效，重复 ready 不会重置正在进行的会话。 */
        if (current == VOICE_AI_LOGIC_STATE_STARTING) {
            atomic_store(&s_state, VOICE_AI_LOGIC_STATE_LISTENING);
        }
        break;

    case VOICE_AI_LOGIC_EVENT_TURN_COMMITTED:
        if (current == VOICE_AI_LOGIC_STATE_LISTENING) {
            atomic_store(&s_state, VOICE_AI_LOGIC_STATE_WAITING_REPLY);
        }
        break;

    case VOICE_AI_LOGIC_EVENT_AUDIO_STARTED:
        if (current == VOICE_AI_LOGIC_STATE_WAITING_REPLY ||
            current == VOICE_AI_LOGIC_STATE_LISTENING) {
            atomic_store(&s_state, VOICE_AI_LOGIC_STATE_PLAYING);
        }
        break;

    case VOICE_AI_LOGIC_EVENT_AUDIO_ENDED:
        /* 音频结束只记录在播放状态中，真正恢复下一轮必须等待 DMA 排空。 */
        break;

    case VOICE_AI_LOGIC_EVENT_PLAYBACK_DRAINED:
        if (current == VOICE_AI_LOGIC_STATE_PLAYING) {
            atomic_store(&s_state, VOICE_AI_LOGIC_STATE_LISTENING);
        }
        break;

    default:
        break;
    }

    return ESP_OK;
}

void voice_ai_logic_reset(void)
{
    atomic_store(&s_state, VOICE_AI_LOGIC_STATE_IDLE);
}

voice_ai_logic_state_t voice_ai_logic_get_state(void)
{
    return (voice_ai_logic_state_t)atomic_load(&s_state);
}

bool voice_ai_logic_is_session_active(void)
{
    return voice_ai_logic_get_state() != VOICE_AI_LOGIC_STATE_IDLE;
}

bool voice_ai_logic_can_capture(void)
{
    return voice_ai_logic_get_state() == VOICE_AI_LOGIC_STATE_LISTENING;
}
