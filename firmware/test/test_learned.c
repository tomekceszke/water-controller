#include <stdlib.h>
#include <string.h>

#include "learned.h"
#include "unity_lite.h"

#define K 410
#define MON 0
#define TUE 1

/* A config in the server's format: every hour dur, vol, the given night limit */
static char *make_json(int dur, int vol, int night, int n_items)
{
    size_t cap = 16384, len = 0;
    char *j = malloc(cap);
    len += snprintf(j + len, cap - len, "{\"v\":1,\"generated\":\"2026-09-29\",\"model\":\"abc\",\"night_flows\":%d", night);
    const char *keys[] = {"dur_s", "vol_l", "exp_l", "p90_l"};
    const int vals[] = {dur, vol, 123, 456};
    for (int k = 0; k < 4; k++) {
        len += snprintf(j + len, cap - len, ",\"%s\":[", keys[k]);
        for (int i = 0; i < n_items; i++) len += snprintf(j + len, cap - len, "%s%d", i ? "," : "", vals[k]);
        len += snprintf(j + len, cap - len, "]");
    }
    snprintf(j + len, cap - len, "}");
    return j;
}

static learned_config_t config(int dur, int vol, int night)
{
    learned_config_t c;
    char *j = make_json(dur, vol, night, LEARNED_HOURS);
    CHECK(learned_parse(j, &c));
    free(j);
    return c;
}

/* seconds of flow at lpm in 1 s samples; returns the first decision with a hit */
static learned_decision_t flow(learned_state_t *s, const learned_config_t *c, int64_t *t, int seconds, double lpm,
                               int dow, int hour, int yday)
{
    for (int i = 0; i < seconds; i++) {
        *t += 1000;
        learned_sample_t smp = {.now_ms = *t, .pulses = (uint32_t) (lpm * K / 60), .flowing = lpm > 0,
                                .local_dow = dow, .local_hour = hour, .local_yday = yday, .pulses_per_liter = K};
        learned_decision_t d = learned_update(s, c, &smp);
        if (d.hit != LEARNED_NONE) return d;
    }
    return (learned_decision_t) {.hit = LEARNED_NONE};
}

static void stop(learned_state_t *s, const learned_config_t *c, int64_t *t)
{
    *t += 3000;
    learned_sample_t smp = {.now_ms = *t, .flowing = false, .pulses_per_liter = K};
    learned_update(s, c, &smp);
}

static void parse_reads_and_clamps(void)
{
    learned_config_t c = config(30, 20000, 1);     // 30 s below the floor, 20000 L above the ceiling
    CHECK(c.valid);
    CHECK_EQ(c.dur_s[0], LEARNED_DUR_MIN_S);
    CHECK_EQ(c.vol_l[167], LEARNED_VOL_MAX_L);
    CHECK_EQ(c.night_flows, LEARNED_NIGHT_FLOWS_MIN);
    CHECK_EQ(c.exp_l_x10[5], 123);
    CHECK_EQ(c.p90_l_x10[100], 456);
    CHECK(strcmp(c.generated, "2026-09-29") == 0);
}

static void parse_rejects_bad_messages(void)
{
    learned_config_t c;
    char *short_arrays = make_json(180, 20, 10, 167);
    CHECK(!learned_parse(short_arrays, &c));
    CHECK(!c.valid);
    free(short_arrays);
    char *long_arrays = make_json(180, 20, 10, 169);
    CHECK(!learned_parse(long_arrays, &c));
    free(long_arrays);
    CHECK(!learned_parse("{\"v\":2}", &c));
    CHECK(!learned_parse("", &c));
    CHECK(!learned_parse("{\"v\":1,\"dur_s\":[1,2,3]}", &c));
}

static void duration_notifies_once_per_flow(void)
{
    learned_config_t c = config(180, 10000, 10);
    learned_state_t s;
    learned_init(&s);
    int64_t t = 0;
    learned_decision_t d = flow(&s, &c, &t, 900, 6, TUE, 3, 100);   // 15 min at 03:00
    CHECK_EQ(d.hit, LEARNED_DURATION);
    CHECK_EQ(d.limit, 180);
    CHECK_EQ(d.value, 181);
    CHECK_EQ(d.hour, 3);
    d = flow(&s, &c, &t, 600, 6, TUE, 3, 100);                      // same flow goes on: no repeat
    CHECK_EQ(d.hit, LEARNED_NONE);
}

static void volume_notifies(void)
{
    learned_config_t c = config(3600, 20, 10);
    learned_state_t s;
    learned_init(&s);
    int64_t t = 0;
    learned_decision_t d = flow(&s, &c, &t, 600, 12, MON, 8, 10);   // 12 L/min: 20 L after 100 s
    CHECK_EQ(d.hit, LEARNED_VOLUME);
    CHECK_EQ(d.limit, 20);
    CHECK(t >= 100000 && t <= 102000);
}

static void limit_follows_the_start_hour(void)
{
    learned_config_t c = config(3600, 10000, 10);
    c.dur_s[learned_how(MON, 5)] = 120;         // strict at 05:00, lenient at 06:00
    learned_state_t s;
    learned_init(&s);
    int64_t t = 0;
    // The flow starts at 05:59 (hour 5) and runs into hour 6: the 05:00 limit applies
    learned_decision_t d = flow(&s, &c, &t, 60, 6, MON, 5, 10);
    CHECK_EQ(d.hit, LEARNED_NONE);
    d = flow(&s, &c, &t, 120, 6, MON, 6, 10);
    CHECK_EQ(d.hit, LEARNED_DURATION);
    CHECK_EQ(d.hour, 5);
}

static void new_flow_rearms(void)
{
    learned_config_t c = config(60, 10000, 10);
    learned_state_t s;
    learned_init(&s);
    int64_t t = 0;
    CHECK_EQ(flow(&s, &c, &t, 120, 6, MON, 12, 10).hit, LEARNED_DURATION);
    stop(&s, &c, &t);
    CHECK_EQ(flow(&s, &c, &t, 120, 6, MON, 12, 10).hit, LEARNED_DURATION);
}

static void night_flows_count_and_reset(void)
{
    learned_config_t c = config(3600, 10000, 3);
    learned_state_t s;
    learned_init(&s);
    int64_t t = 0;
    for (int i = 0; i < 3; i++) {                           // three 1 L flows at 02:00: at the limit, quiet
        CHECK_EQ(flow(&s, &c, &t, 10, 6, TUE, 2, 50).hit, LEARNED_NONE);
        stop(&s, &c, &t);
    }
    learned_decision_t d = flow(&s, &c, &t, 10, 6, TUE, 2, 50);   // the fourth notifies
    CHECK_EQ(d.hit, LEARNED_NIGHT_FLOWS);
    CHECK_EQ(d.value, 4);
    CHECK_EQ(d.limit, 3);
    stop(&s, &c, &t);
    CHECK_EQ(flow(&s, &c, &t, 10, 6, TUE, 3, 50).hit, LEARNED_NONE);   // once per night
    stop(&s, &c, &t);
    CHECK_EQ(flow(&s, &c, &t, 10, 6, TUE, 14, 50).hit, LEARNED_NONE);  // day flows do not count
    stop(&s, &c, &t);
    for (int i = 0; i < 3; i++) {                           // next night starts from zero
        CHECK_EQ(flow(&s, &c, &t, 10, 6, TUE + 1, 1, 51).hit, LEARNED_NONE);
        stop(&s, &c, &t);
    }
}

static void tiny_flows_do_not_count_at_night(void)
{
    learned_config_t c = config(3600, 10000, 3);
    learned_state_t s;
    learned_init(&s);
    int64_t t = 0;
    for (int i = 0; i < 10; i++) {                          // 0.05 L each, below the 0.1 L hc-data counts
        CHECK_EQ(flow(&s, &c, &t, 1, 3, TUE, 2, 50).hit, LEARNED_NONE);
        stop(&s, &c, &t);
    }
}

static void nothing_without_clock_or_config(void)
{
    learned_config_t c = config(60, 5, 3);
    learned_state_t s;
    learned_init(&s);
    int64_t t = 0;
    CHECK_EQ(flow(&s, &c, &t, 600, 12, -1, -1, 0).hit, LEARNED_NONE);     // clock not synced
    stop(&s, &c, &t);
    learned_config_t none = {0};
    CHECK_EQ(flow(&s, &none, &t, 600, 12, MON, 3, 0).hit, LEARNED_NONE);  // no config
    stop(&s, &c, &t);
    CHECK_EQ(flow(&s, NULL, &t, 600, 12, MON, 3, 0).hit, LEARNED_NONE);
}

int main(void)
{
    RUN(parse_reads_and_clamps);
    RUN(parse_rejects_bad_messages);
    RUN(duration_notifies_once_per_flow);
    RUN(volume_notifies);
    RUN(limit_follows_the_start_hour);
    RUN(new_flow_rearms);
    RUN(night_flows_count_and_reset);
    RUN(tiny_flows_do_not_count_at_night);
    RUN(nothing_without_clock_or_config);
    return report();
}
