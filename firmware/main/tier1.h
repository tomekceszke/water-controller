#pragma once

/*
 * Tier 1 protection: close the valve after continuous flow longer than a limit.
 * Pure logic (no ESP-IDF), driven by the flow task and unit-tested on the host.
 *
 * "Continuous" = pulses keep arriving with no pause longer than gap_ms. The limit can be raised or lowered at
 * runtime; it is clamped by the caller (settings) and is never disabled.
 */

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t limit_s;       // continuous flow allowed before the valve closes
    uint32_t gap_ms;        // a pause longer than this ends the flow
} tier1_config_t;

typedef struct {
    bool flowing;
    bool tripped;           // limit reached during the current flow (reported once)
    int64_t start_ms;       // first pulse of the current flow
    int64_t last_pulse_ms;  // sample time of the latest pulses
    uint64_t pulses;        // pulses in the current flow
} tier1_state_t;

typedef enum {
    TIER1_IDLE,             // no flow
    TIER1_STARTED,          // flow began in this sample
    TIER1_FLOWING,          // flow continues, below the limit
    TIER1_ENDED,            // flow ended in this sample (pause > gap); state keeps the finished flow until next start
    TIER1_TRIP,             // limit reached in this sample: close the valve
    TIER1_TRIPPED,          // limit was reached earlier in this flow and water still flows (valve not closing?)
} tier1_result_t;

void tier1_init(tier1_state_t *state);

/* One sample: new_pulses counted since the previous call, now_ms from a monotonic clock. */
tier1_result_t tier1_update(tier1_state_t *state, const tier1_config_t *config, int64_t now_ms, uint32_t new_pulses);

/* Duration of the current (or just ended) flow in ms. */
int64_t tier1_elapsed_ms(const tier1_state_t *state, int64_t now_ms);

/* The valve was opened (by a person): a flow that is still running gets a new, full limit from now. */
void tier1_valve_opened(tier1_state_t *state, int64_t now_ms);

/* Seconds left before a trip, 0 when not flowing or already tripped. */
uint32_t tier1_remaining_s(const tier1_state_t *state, const tier1_config_t *config, int64_t now_ms);
