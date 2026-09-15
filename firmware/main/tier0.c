#include <string.h>

#include "tier0.h"

void tier0_init(tier0_state_t *state)
{
    memset(state, 0, sizeof(*state));
}

void tier0_valve_opened(tier0_state_t *state, int64_t now_ms)
{
    if (!state->flowing) return;
    state->start_ms = now_ms;
    state->tripped = false;
}

bool tier0_update(tier0_state_t *state, int64_t now_ms, uint32_t new_pulses)
{
    if (new_pulses > 0) {
        if (!state->flowing) {
            state->flowing = true;
            state->tripped = false;
            state->start_ms = now_ms;
        }
        state->last_pulse_ms = now_ms;
    } else if (state->flowing && now_ms - state->last_pulse_ms > TIER0_GAP_MS) {
        state->flowing = false;
        return false;
    }
    if (!state->flowing || state->tripped) return false;
    if (now_ms - state->start_ms >= (int64_t) TIER0_LIMIT_S * 1000) {
        state->tripped = true;
        return true;
    }
    return false;
}
