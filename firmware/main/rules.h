#pragma once

/*
 * Tier 2a protection: local anomaly rules. Pure logic (no ESP-IDF), unit-tested on the host.
 * Runs in its own task on flow samples; it can only request a close or a notification. Tier 1 never depends on it.
 */

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t pulses_per_liter;      // calibration
    uint32_t max_event_liters;      // one continuous flow above this closes; 0 = off
    uint32_t burst_lpm;             // flow rate above this ...
    uint32_t burst_s;               // ... for longer than this closes; 0 = off
    uint8_t night_start_h;          // local hour, window [start, end) may wrap midnight
    uint8_t night_end_h;
    uint32_t night_max_liters;      // one flow above this inside the window closes; 0 = off
    uint32_t leak_notify_min;       // water active in every minute for this long notifies; 0 = off
    bool vacation;
    uint32_t vacation_max_liters;   // with vacation on, one flow above this closes
} rules_config_t;

typedef struct {
    int64_t now_ms;                 // monotonic
    int local_hour;                 // 0-23, or -1 when the wall clock is not synced
    uint32_t pulses;                // new pulses since the previous sample
    uint32_t sample_ms;             // length of the sample window (for the rate)
    bool flowing;                   // Tier 1 view of the current flow
} rules_sample_t;

typedef enum {
    RULE_NONE = 0,
    RULE_MAX_VOLUME,
    RULE_BURST,
    RULE_NIGHT,
    RULE_VACATION,
    RULE_LEAK,                      // notification only
} rule_t;

typedef struct {
    bool close;
    bool notify;
    rule_t rule;
    uint32_t liters;                // volume of the flow that triggered the rule
    uint32_t lpm;                   // rate at the trigger
} rules_decision_t;

typedef struct {
    bool flowing;
    uint64_t event_pulses;          // current flow
    uint64_t night_pulses;          // current flow, inside the night window
    int64_t burst_since_ms;         // 0 = rate below threshold
    int64_t active_minute_start_ms; // start of the running minute
    bool minute_active;             // pulses seen in the running minute
    uint32_t active_minutes;        // consecutive active minutes
    bool leak_notified;
    bool closed_this_flow;          // one close request per flow
    int64_t snooze_until_ms;        // rules except vacation are muted until then
} rules_state_t;

void rules_init(rules_state_t *state);

rules_decision_t rules_update(rules_state_t *state, const rules_config_t *config, const rules_sample_t *sample);

/* Mutes volume, burst and night rules for minutes (0 cancels). Vacation stays armed. */
void rules_snooze(rules_state_t *state, int64_t now_ms, uint32_t minutes);

uint32_t rules_snooze_left_s(const rules_state_t *state, int64_t now_ms);

const char *rules_name(rule_t rule);
