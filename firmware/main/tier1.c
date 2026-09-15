#include <string.h>

#include "tier1.h"

void tier1_init(tier1_state_t *state)
{
    memset(state, 0, sizeof(*state));
}

int64_t tier1_elapsed_ms(const tier1_state_t *state, int64_t now_ms)
{
    if (state->start_ms == 0 && !state->flowing) return 0;
    int64_t end = state->flowing ? now_ms : state->last_pulse_ms;
    return end > state->start_ms ? end - state->start_ms : 0;
}

uint32_t tier1_remaining_s(const tier1_state_t *state, const tier1_config_t *config, int64_t now_ms)
{
    if (!state->flowing || state->tripped) return 0;
    int64_t left_ms = (int64_t) config->limit_s * 1000 - (now_ms - state->start_ms);
    return left_ms > 0 ? (uint32_t) ((left_ms + 999) / 1000) : 0;
}

tier1_result_t tier1_update(tier1_state_t *state, const tier1_config_t *config, int64_t now_ms, uint32_t new_pulses)
{
    if (new_pulses > 0) {
        if (!state->flowing) {
            state->flowing = true;
            state->tripped = false;
            state->start_ms = now_ms;
            state->pulses = 0;
        }
        state->last_pulse_ms = now_ms;
        state->pulses += new_pulses;
    } else if (state->flowing && now_ms - state->last_pulse_ms > (int64_t) config->gap_ms) {
        state->flowing = false;
        return TIER1_ENDED;
    }

    if (!state->flowing) return TIER1_IDLE;

    if (state->tripped) return TIER1_TRIPPED;
    // Evaluated on every sample while flowing, also during a short pause inside the gap
    if (now_ms - state->start_ms >= (int64_t) config->limit_s * 1000) {
        state->tripped = true;
        return TIER1_TRIP;
    }
    return state->pulses == new_pulses && new_pulses > 0 && state->start_ms == now_ms ? TIER1_STARTED : TIER1_FLOWING;
}
