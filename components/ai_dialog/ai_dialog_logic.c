/**
 * @file ai_dialog_logic.c
 * @brief AI 对话状态机实现。
 */

#include "ai_dialog_logic.h"

#include <stdatomic.h>

/*
 * 状态只由本文件维护。应用层的编码任务、网络回调和播放任务通过原子读
 * 取得状态，所有状态迁移统一经过 ai_dialog_logic_handle_event()。
 */
static atomic_int s_state = AI_DIALOG_LOGIC_STATE_IDLE;
static atomic_bool s_initialized = false;

esp_err_t ai_dialog_logic_init(void)
{
    atomic_store(&s_state, AI_DIALOG_LOGIC_STATE_IDLE);
    atomic_store(&s_initialized, true);
    return ESP_OK;
}

static bool event_is_reset(ai_dialog_logic_event_t event)
{
    return event == AI_DIALOG_LOGIC_EVENT_BACKEND_ERROR ||
           event == AI_DIALOG_LOGIC_EVENT_BACKEND_STOPPED ||
           event == AI_DIALOG_LOGIC_EVENT_STOP;
}

esp_err_t ai_dialog_logic_handle_event(ai_dialog_logic_event_t event)
{
    if (!atomic_load(&s_initialized)) {
        return ESP_ERR_INVALID_STATE;
    }

    ai_dialog_logic_state_t current = (ai_dialog_logic_state_t)atomic_load(&s_state);
    if (event_is_reset(event)) {
        atomic_store(&s_state, AI_DIALOG_LOGIC_STATE_IDLE);
        return ESP_OK;
    }

    switch (event) {
    case AI_DIALOG_LOGIC_EVENT_WAKE_DETECTED:
        /* 只有待机状态接受新的唤醒，避免重复唤醒创建并行会话。 */
        if (current == AI_DIALOG_LOGIC_STATE_IDLE) {
            atomic_store(&s_state, AI_DIALOG_LOGIC_STATE_STARTING);
        }
        break;

    case AI_DIALOG_LOGIC_EVENT_BACKEND_READY:
        /* 后端就绪只对本次唤醒有效，重复 ready 不会重置正在进行的会话。 */
        if (current == AI_DIALOG_LOGIC_STATE_STARTING) {
            atomic_store(&s_state, AI_DIALOG_LOGIC_STATE_LISTENING);
        }
        break;

    case AI_DIALOG_LOGIC_EVENT_TURN_COMMITTED:
        if (current == AI_DIALOG_LOGIC_STATE_LISTENING) {
            atomic_store(&s_state, AI_DIALOG_LOGIC_STATE_WAITING_REPLY);
        }
        break;

    case AI_DIALOG_LOGIC_EVENT_AUDIO_STARTED:
        if (current == AI_DIALOG_LOGIC_STATE_WAITING_REPLY ||
            current == AI_DIALOG_LOGIC_STATE_LISTENING) {
            atomic_store(&s_state, AI_DIALOG_LOGIC_STATE_PLAYING);
        }
        break;

    case AI_DIALOG_LOGIC_EVENT_AUDIO_ENDED:
        /* 音频结束只记录在播放状态中，真正恢复下一轮必须等待 DMA 排空。 */
        break;

    case AI_DIALOG_LOGIC_EVENT_PLAYBACK_DRAINED:
        if (current == AI_DIALOG_LOGIC_STATE_PLAYING) {
            atomic_store(&s_state, AI_DIALOG_LOGIC_STATE_LISTENING);
        }
        break;

    default:
        break;
    }

    return ESP_OK;
}

void ai_dialog_logic_reset(void)
{
    atomic_store(&s_state, AI_DIALOG_LOGIC_STATE_IDLE);
}

ai_dialog_logic_state_t ai_dialog_logic_get_state(void)
{
    return (ai_dialog_logic_state_t)atomic_load(&s_state);
}

bool ai_dialog_logic_is_session_active(void)
{
    return ai_dialog_logic_get_state() != AI_DIALOG_LOGIC_STATE_IDLE;
}

bool ai_dialog_logic_can_capture(void)
{
    return ai_dialog_logic_get_state() == AI_DIALOG_LOGIC_STATE_LISTENING;
}
