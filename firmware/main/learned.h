#pragma once

/*
 * Tier 2b: limits learned on hc-data (server/model), delivered as the retained MQTT water/<mac>/config.
 * Pure logic (no ESP-IDF), unit-tested on the host. Notification only: it never requests a close, and without a
 * valid config it does nothing, so the device behaves exactly as before.
 *
 * Hours are local hours of week, Monday 00:00 = 0 ... Sunday 23:00 = 167.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LEARNED_HOURS 168
#define LEARNED_NIGHT_END_H 6           // "night" for the night flow count: local 00:00-05:59

/* Hard bounds: every value from the server is clamped into them */
#define LEARNED_DUR_MIN_S 60
#define LEARNED_DUR_MAX_S 3600
#define LEARNED_VOL_MIN_L 5
#define LEARNED_VOL_MAX_L 10000
#define LEARNED_NIGHT_FLOWS_MIN 3
#define LEARNED_NIGHT_FLOWS_MAX 200
#define LEARNED_EXP_MAX_X10 50000       // expected liters x10

typedef struct {
    bool valid;
    char generated[11];                 // YYYY-MM-DD, as sent by the server
    uint16_t dur_s[LEARNED_HOURS];      // one flow longer than this notifies
    uint16_t vol_l[LEARNED_HOURS];      // one flow bigger than this notifies
    uint16_t night_flows;               // more flows (>= 0.1 L) than this starting 00-06 notifies
    uint16_t exp_l_x10[LEARNED_HOURS];  // expected liters per hour x10 (History chart only)
    uint16_t p90_l_x10[LEARNED_HOURS];
} learned_config_t;

typedef struct {
    int64_t now_ms;                     // monotonic
    uint32_t pulses;                    // new pulses since the previous sample
    bool flowing;                       // Tier 1 view of the current flow
    int local_dow;                      // 0 = Monday ... 6 = Sunday; -1 when the clock is not synced
    int local_hour;                     // 0-23; -1 when the clock is not synced
    int local_yday;                     // day of year, to tell one night from the next
    uint32_t pulses_per_liter;
    uint32_t max_dur_s;                 // cap on the duration limit (below the Tier 1 shut-off), 0 = none
    uint32_t max_vol_l;                 // cap on the volume limit (rules.h tier2_volume_cap_l), 0 = none
} learned_sample_t;

typedef enum {
    LEARNED_NONE = 0,
    LEARNED_DURATION,
    LEARNED_VOLUME,
    LEARNED_NIGHT_FLOWS,
} learned_hit_t;

typedef struct {
    learned_hit_t hit;
    uint32_t value;                     // seconds, liters or flows
    uint32_t limit;                     // same unit
    int8_t hour;                        // local hour the limit belongs to (flow start / night)
} learned_decision_t;

typedef struct {
    bool flowing;
    int64_t start_ms;
    uint64_t pulses;
    int how;                            // hour of week at the flow start, -1 = unknown (clock not synced)
    bool dur_done, vol_done, counted;
    int night_yday;                     // night being counted, -1 = none
    uint32_t night_flows;
    bool night_done;
} learned_state_t;

/* Parses and clamps a NUL-terminated config. Returns false (and leaves *out invalid) when the version is not 1 or
 * an array does not have exactly LEARNED_HOURS numbers. */
bool learned_parse(const char *json, learned_config_t *out);

void learned_init(learned_state_t *state);

/* One decision per sample at most; each flow notifies once per indicator, each night once. */
learned_decision_t learned_update(learned_state_t *state, const learned_config_t *config, const learned_sample_t *s);

/* Duration limit for an hour of week after the cap: a notification that would come after Tier 1 has already shut
 * the water off tells nothing, so the limit stays below it (never under LEARNED_DUR_MIN_S). */
static inline uint32_t learned_dur_limit(const learned_config_t *c, int how, uint32_t max_dur_s)
{
    uint32_t limit = c->dur_s[how];
    if (max_dur_s && max_dur_s < limit) limit = max_dur_s < LEARNED_DUR_MIN_S ? LEARNED_DUR_MIN_S : max_dur_s;
    return limit;
}

static inline uint32_t learned_vol_limit(const learned_config_t *c, int how, uint32_t max_vol_l)
{
    uint32_t limit = c->vol_l[how];
    if (max_vol_l && max_vol_l < limit) limit = max_vol_l < LEARNED_VOL_MIN_L ? LEARNED_VOL_MIN_L : max_vol_l;
    return limit;
}

static inline int learned_how(int dow, int hour)
{
    return (dow < 0 || hour < 0) ? -1 : dow * 24 + hour;
}
