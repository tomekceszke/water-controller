#include <stdlib.h>
#include <string.h>

#include "learned.h"

/* Minimal reader for the one flat JSON object the server publishes: finds "key": and reads what follows. It is
 * not a general JSON parser, but nothing it accepts can leave a value outside the hard bounds. */
static const char *find_value(const char *json, const char *key)
{
    size_t klen = strlen(key);
    for (const char *p = json; (p = strchr(p, '"')) != NULL; p++) {
        if (strncmp(p + 1, key, klen) == 0 && p[klen + 1] == '"') {
            const char *v = p + klen + 2;
            while (*v == ' ' || *v == '\t' || *v == '\n' || *v == '\r') v++;
            if (*v != ':') continue;
            v++;
            while (*v == ' ' || *v == '\t' || *v == '\n' || *v == '\r') v++;
            return v;
        }
    }
    return NULL;
}

static long clamp(long v, long lo, long hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

static bool read_array(const char *json, const char *key, uint16_t *out, long lo, long hi)
{
    const char *p = find_value(json, key);
    if (p == NULL || *p != '[') return false;
    p++;
    for (int i = 0; i < LEARNED_HOURS; i++) {
        while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') p++;
        char *end;
        double v = strtod(p, &end);
        if (end == p) return false;
        out[i] = (uint16_t) clamp((long) (v + 0.5), lo, hi);
        p = end;
        while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') p++;
        if (i < LEARNED_HOURS - 1) {
            if (*p != ',') return false;
            p++;
        }
    }
    return *p == ']';
}

bool learned_parse(const char *json, learned_config_t *out)
{
    memset(out, 0, sizeof(*out));
    const char *v = find_value(json, "v");
    if (v == NULL || strtol(v, NULL, 10) != 1) return false;

    learned_config_t c = {0};
    if (!read_array(json, "dur_s", c.dur_s, LEARNED_DUR_MIN_S, LEARNED_DUR_MAX_S)) return false;
    if (!read_array(json, "vol_l", c.vol_l, LEARNED_VOL_MIN_L, LEARNED_VOL_MAX_L)) return false;
    if (!read_array(json, "exp_l", c.exp_l_x10, 0, LEARNED_EXP_MAX_X10)) return false;
    if (!read_array(json, "p90_l", c.p90_l_x10, 0, LEARNED_EXP_MAX_X10)) return false;
    const char *n = find_value(json, "night_flows");
    if (n == NULL) return false;
    char *end;
    long nf = strtol(n, &end, 10);
    if (end == n) return false;
    c.night_flows = (uint16_t) clamp(nf, LEARNED_NIGHT_FLOWS_MIN, LEARNED_NIGHT_FLOWS_MAX);

    const char *g = find_value(json, "generated");
    if (g != NULL && *g == '"') {
        size_t i = 0;
        for (g++; *g && *g != '"' && i < sizeof(c.generated) - 1; g++) {
            if ((*g >= '0' && *g <= '9') || *g == '-') c.generated[i++] = *g;
        }
    }
    c.valid = true;
    *out = c;
    return true;
}

void learned_init(learned_state_t *state)
{
    memset(state, 0, sizeof(*state));
    state->how = -1;
    state->night_yday = -1;
}

learned_decision_t learned_update(learned_state_t *st, const learned_config_t *c, const learned_sample_t *s)
{
    learned_decision_t d = {.hit = LEARNED_NONE, .hour = -1};
    if (!s->flowing) {
        st->flowing = false;
        return d;
    }
    if (!st->flowing) {
        st->flowing = true;
        st->start_ms = s->now_ms;
        st->pulses = 0;
        st->how = learned_how(s->local_dow, s->local_hour);
        st->dur_done = st->vol_done = st->counted = false;
    }
    st->pulses += s->pulses;
    if (c == NULL || !c->valid || st->how < 0 || s->pulses_per_liter == 0) return d;

    const int hour = st->how % 24;
    // A new flow of at least 0.1 L (the size hc-data counts) that started at night
    if (!st->counted && st->pulses * 10 >= s->pulses_per_liter) {
        st->counted = true;
        if (hour < LEARNED_NIGHT_END_H) {
            if (st->night_yday != s->local_yday) {
                st->night_yday = s->local_yday;
                st->night_flows = 0;
                st->night_done = false;
            }
            st->night_flows++;
            if (!st->night_done && st->night_flows > c->night_flows) {
                st->night_done = true;
                return (learned_decision_t) {LEARNED_NIGHT_FLOWS, st->night_flows, c->night_flows, (int8_t) hour};
            }
        }
    }
    const uint32_t elapsed_s = (uint32_t) ((s->now_ms - st->start_ms) / 1000);
    const uint32_t dur_limit = learned_dur_limit(c, st->how, s->max_dur_s);
    if (!st->dur_done && elapsed_s > dur_limit) {
        st->dur_done = true;
        return (learned_decision_t) {LEARNED_DURATION, elapsed_s, dur_limit, (int8_t) hour};
    }
    const uint32_t vol_limit = learned_vol_limit(c, st->how, s->max_vol_l);
    const uint64_t limit_pulses = (uint64_t) vol_limit * s->pulses_per_liter;
    if (!st->vol_done && st->pulses > limit_pulses) {
        st->vol_done = true;
        return (learned_decision_t) {LEARNED_VOLUME, (uint32_t) (st->pulses / s->pulses_per_liter), vol_limit,
                                     (int8_t) hour};
    }
    return d;
}

bool learned_closes(learned_hit_t hit, int hour, bool close_night)
{
    return close_night && (hit == LEARNED_DURATION || hit == LEARNED_VOLUME)
           && hour >= LEARNED_CLOSE_START_H && hour < LEARNED_NIGHT_END_H;
}
