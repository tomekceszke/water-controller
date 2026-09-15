#include <string.h>

#include "rules.h"

#define MINUTE_MS 60000

void rules_init(rules_state_t *state)
{
    memset(state, 0, sizeof(*state));
}

void rules_snooze(rules_state_t *state, int64_t now_ms, uint32_t minutes)
{
    state->snooze_until_ms = minutes ? now_ms + (int64_t) minutes * MINUTE_MS : 0;
}

uint32_t rules_snooze_left_s(const rules_state_t *state, int64_t now_ms)
{
    int64_t left = state->snooze_until_ms - now_ms;
    return left > 0 ? (uint32_t) ((left + 999) / 1000) : 0;
}

const char *rules_name(rule_t rule)
{
    switch (rule) {
        case RULE_MAX_VOLUME: return "max_volume";
        case RULE_BURST:      return "burst";
        case RULE_NIGHT:      return "night";
        case RULE_VACATION:   return "vacation";
        case RULE_LEAK:       return "leak";
        default:              return "none";
    }
}

static bool in_night(const rules_config_t *config, int hour)
{
    if (hour < 0 || config->night_start_h == config->night_end_h) return false;
    if (config->night_start_h < config->night_end_h) {
        return hour >= config->night_start_h && hour < config->night_end_h;
    }
    return hour >= config->night_start_h || hour < config->night_end_h;     // wraps midnight
}

static uint32_t liters(const rules_config_t *config, uint64_t pulses)
{
    return config->pulses_per_liter ? (uint32_t) (pulses / config->pulses_per_liter) : 0;
}

static void track_leak_minutes(rules_state_t *state, const rules_config_t *config, const rules_sample_t *s,
                               rules_decision_t *d)
{
    if (state->active_minute_start_ms == 0) state->active_minute_start_ms = s->now_ms;
    if (s->pulses > 0) state->minute_active = true;

    while (s->now_ms - state->active_minute_start_ms >= MINUTE_MS) {
        if (state->minute_active) {
            state->active_minutes++;
        } else {
            state->active_minutes = 0;
            state->leak_notified = false;
        }
        state->minute_active = false;
        state->active_minute_start_ms += MINUTE_MS;
    }
    if (config->leak_notify_min && !state->leak_notified && state->active_minutes >= config->leak_notify_min) {
        state->leak_notified = true;
        d->notify = true;
        d->rule = RULE_LEAK;
    }
}

rules_decision_t rules_update(rules_state_t *state, const rules_config_t *config, const rules_sample_t *s)
{
    rules_decision_t d = {0};

    track_leak_minutes(state, config, s, &d);

    if (!s->flowing) {
        state->flowing = false;
        state->event_pulses = 0;
        state->night_pulses = 0;
        state->burst_since_ms = 0;
        state->closed_this_flow = false;
        return d;
    }
    if (!state->flowing) {
        state->flowing = true;
        state->event_pulses = 0;
        state->night_pulses = 0;
        state->burst_since_ms = 0;
        state->closed_this_flow = false;
    }
    state->event_pulses += s->pulses;
    if (in_night(config, s->local_hour)) state->night_pulses += s->pulses;

    uint32_t lpm = 0;
    if (s->sample_ms > 0 && config->pulses_per_liter > 0) {
        lpm = (uint32_t) ((uint64_t) s->pulses * 60000u / ((uint64_t) s->sample_ms * config->pulses_per_liter));
    }
    if (config->burst_s && lpm > config->burst_lpm) {
        if (state->burst_since_ms == 0) state->burst_since_ms = s->now_ms;
    } else {
        state->burst_since_ms = 0;
    }

    if (state->closed_this_flow) return d;

    uint32_t event_l = liters(config, state->event_pulses);
    uint64_t ppl = config->pulses_per_liter;
    bool snoozed = s->now_ms < state->snooze_until_ms;
    rule_t hit = RULE_NONE;
    // Limits compare pulses, not whole liters: "above 5 L" fires at 5.0 L + 1 pulse, not at 6 L
    if (config->vacation && state->event_pulses > config->vacation_max_liters * ppl) {
        hit = RULE_VACATION;
    } else if (!snoozed && config->max_event_liters && state->event_pulses > config->max_event_liters * ppl) {
        hit = RULE_MAX_VOLUME;
    } else if (!snoozed && config->burst_s && state->burst_since_ms != 0
               && s->now_ms - state->burst_since_ms >= (int64_t) config->burst_s * 1000) {
        hit = RULE_BURST;
    } else if (!snoozed && config->night_max_liters && state->night_pulses > config->night_max_liters * ppl) {
        hit = RULE_NIGHT;
    }
    if (hit != RULE_NONE) {
        state->closed_this_flow = true;
        d.close = true;
        d.notify = true;
        d.rule = hit;
    }
    d.liters = event_l;
    d.lpm = lpm;
    return d;
}
