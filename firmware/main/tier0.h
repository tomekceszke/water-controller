#pragma once

/*
 * Tier 0 protection: a hard-coded ceiling for continuous flow, deliberately NOT configurable.
 * It guards against anything going wrong with Tier 1 (a bad setting, a bug in its logic or in settings/NVS):
 * no setting, API, snooze or NVS value can raise or disable it. Pure logic, unit-tested on the host.
 *
 * Own flow tracking, independent of Tier 1: a pause longer than TIER0_GAP_MS ends the flow and resets the timer.
 * After a trip the valve closes, the water stops and the flow ends; turning the water back on starts a new
 * flow with a fresh hour (e.g. filling a pool: turn the water on again every hour). If the valve is opened again
 * before the water stopped, tier0_valve_opened() starts the new hour there, so a reopened flow is never unguarded.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef WATER_TEST_TIER0_S
#define TIER0_LIMIT_S   WATER_TEST_TIER0_S      // hardware test builds only
#else
#define TIER0_LIMIT_S   3600                    // 60 minutes of continuous flow
#endif
#define TIER0_GAP_MS    5000                    // more tolerant than Tier 1: a slow trickle still counts as flowing

typedef struct {
    bool flowing;
    bool tripped;
    int64_t start_ms;
    int64_t last_pulse_ms;
} tier0_state_t;

void tier0_init(tier0_state_t *state);

/* True exactly once per flow, in the sample where continuous flow reaches TIER0_LIMIT_S. */
bool tier0_update(tier0_state_t *state, int64_t now_ms, uint32_t new_pulses);

/* The valve was opened (by a person): a flow that is still running gets a new, full limit from now. */
void tier0_valve_opened(tier0_state_t *state, int64_t now_ms);
