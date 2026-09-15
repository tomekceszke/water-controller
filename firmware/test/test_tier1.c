#include "tier1.h"
#include "unity_lite.h"

static const tier1_config_t CFG = {.limit_s = 60, .gap_ms = 2000};

/* Feeds samples every 1000 ms from t0 with the given pulses per sample; returns the last result. */
static tier1_result_t feed(tier1_state_t *s, int64_t *t, int samples, uint32_t pulses)
{
    tier1_result_t r = TIER1_IDLE;
    for (int i = 0; i < samples; i++) {
        *t += 1000;
        r = tier1_update(s, &CFG, *t, pulses);
    }
    return r;
}

static void idle_without_pulses(void)
{
    tier1_state_t s;
    tier1_init(&s);
    int64_t t = 1000;
    CHECK_EQ(feed(&s, &t, 100, 0), TIER1_IDLE);
    CHECK(!s.flowing);
}

static void start_flow_and_end_after_gap(void)
{
    tier1_state_t s;
    tier1_init(&s);
    int64_t t = 1000;
    CHECK_EQ(feed(&s, &t, 1, 50), TIER1_STARTED);
    CHECK_EQ(feed(&s, &t, 5, 50), TIER1_FLOWING);
    CHECK_EQ(s.pulses, 300);
    CHECK_EQ(feed(&s, &t, 2, 0), TIER1_FLOWING);    // 2 s pause == gap: still flowing
    CHECK_EQ(feed(&s, &t, 1, 0), TIER1_ENDED);      // 3 s pause > gap
    CHECK_EQ(tier1_elapsed_ms(&s, t), 5000);        // from first to last pulse
    CHECK_EQ(feed(&s, &t, 1, 0), TIER1_IDLE);
}

static void trips_once_at_limit(void)
{
    tier1_state_t s;
    tier1_init(&s);
    int64_t t = 1000;
    feed(&s, &t, 1, 10);                            // start at t=2000
    CHECK_EQ(feed(&s, &t, 59, 10), TIER1_FLOWING);  // 59 s elapsed
    CHECK_EQ(tier1_remaining_s(&s, &CFG, t), 1);
    CHECK_EQ(feed(&s, &t, 1, 10), TIER1_TRIP);      // 60 s
    CHECK_EQ(feed(&s, &t, 5, 10), TIER1_TRIPPED);   // valve still closing
    CHECK_EQ(tier1_remaining_s(&s, &CFG, t), 0);
    CHECK_EQ(feed(&s, &t, 3, 0), TIER1_ENDED);
}

static void short_pauses_do_not_reset_the_timer(void)
{
    tier1_state_t s;
    tier1_init(&s);
    int64_t t = 1000;
    tier1_result_t r = TIER1_IDLE;
    // Trickle: one pulse every 2 s for 70 s
    for (int i = 0; i < 70 && r != TIER1_TRIP; i++) {
        t += 1000;
        r = tier1_update(&s, &CFG, t, (i % 2) ? 1 : 0);
    }
    CHECK_EQ(r, TIER1_TRIP);
}

static void trip_during_pause_inside_gap(void)
{
    tier1_state_t s;
    tier1_init(&s);
    int64_t t = 1000;
    feed(&s, &t, 1, 10);
    feed(&s, &t, 58, 10);                           // 58 s
    CHECK_EQ(feed(&s, &t, 2, 0), TIER1_TRIP);       // 60 s reached while pausing (within gap)
}

static void new_flow_after_trip_starts_fresh(void)
{
    tier1_state_t s;
    tier1_init(&s);
    int64_t t = 1000;
    feed(&s, &t, 61, 10);
    CHECK(s.tripped);
    feed(&s, &t, 3, 0);
    CHECK_EQ(feed(&s, &t, 1, 5), TIER1_STARTED);
    CHECK(!s.tripped);
    CHECK_EQ(s.pulses, 5);
}

static void limit_change_applies_to_running_flow(void)
{
    tier1_state_t s;
    tier1_init(&s);
    tier1_config_t cfg = {.limit_s = 600, .gap_ms = 2000};
    int64_t t = 1000;
    for (int i = 0; i < 100; i++) tier1_update(&s, &cfg, t += 1000, 5);
    cfg.limit_s = 90;                               // lowered from the app during the flow
    CHECK_EQ(tier1_update(&s, &cfg, t += 1000, 5), TIER1_TRIP);
}

static void large_pulse_counts_do_not_overflow(void)
{
    tier1_state_t s;
    tier1_init(&s);
    tier1_config_t cfg = {.limit_s = 14400, .gap_ms = 2000};
    int64_t t = 1000;
    for (int i = 0; i < 7200; i++) tier1_update(&s, &cfg, t += 1000, 400);     // 2 h at ~50 L/min
    CHECK_EQ(s.pulses, 2880000ULL);
    CHECK(s.flowing);
}

static void reopen_during_running_flow_restarts_the_limit(void)
{
    tier1_state_t s;
    tier1_init(&s);
    int64_t t = 1000;
    feed(&s, &t, 61, 10);               // trip at 60 s
    CHECK(s.tripped);
    tier1_valve_opened(&s, t);          // reopened while water still runs
    CHECK(!s.tripped);
    CHECK_EQ(feed(&s, &t, 59, 10), TIER1_FLOWING);
    CHECK_EQ(feed(&s, &t, 1, 10), TIER1_TRIP);
}

int main(void)
{
    RUN(idle_without_pulses);
    RUN(start_flow_and_end_after_gap);
    RUN(trips_once_at_limit);
    RUN(short_pauses_do_not_reset_the_timer);
    RUN(trip_during_pause_inside_gap);
    RUN(new_flow_after_trip_starts_fresh);
    RUN(limit_change_applies_to_running_flow);
    RUN(large_pulse_counts_do_not_overflow);
    RUN(reopen_during_running_flow_restarts_the_limit);
    return report();
}
