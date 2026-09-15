#include "tier0.h"
#include "unity_lite.h"

static int64_t t;

static bool feed(tier0_state_t *s, int seconds, uint32_t pulses)
{
    bool tripped = false;
    for (int i = 0; i < seconds; i++) {
        t += 1000;
        tripped |= tier0_update(s, t, pulses);
    }
    return tripped;
}

static void limit_is_one_hour(void)
{
    CHECK_EQ(TIER0_LIMIT_S, 3600);
}

static void trips_after_an_hour_once(void)
{
    tier0_state_t s;
    tier0_init(&s);
    t = 0;
    CHECK(!feed(&s, 3600, 100));        // timer starts at the first pulse: 3599 s elapsed
    CHECK(feed(&s, 1, 100));            // 3600 s
    CHECK(!feed(&s, 120, 100));         // valve still closing: no second trip
}

static void trickle_with_short_pauses_is_continuous(void)
{
    tier0_state_t s;
    tier0_init(&s);
    t = 0;
    bool tripped = false;
    for (int i = 0; i < 3700 && !tripped; i++) {
        t += 1000;
        tripped = tier0_update(&s, t, (i % 5 == 0) ? 1 : 0);  // one pulse every 5 s
    }
    CHECK(tripped);
}

static void pause_longer_than_gap_restarts_the_hour(void)
{
    tier0_state_t s;
    tier0_init(&s);
    t = 0;
    CHECK(!feed(&s, 3000, 100));
    CHECK(!feed(&s, 6, 0));             // 6 s without water: the flow ended
    CHECK(!feed(&s, 3000, 100));        // 50 min again
    CHECK(!feed(&s, 600, 100));         // 59:59
    CHECK(feed(&s, 1, 100));
}

static void water_turned_on_again_after_a_trip_gets_a_new_hour(void)
{
    tier0_state_t s;
    tier0_init(&s);
    t = 0;
    CHECK(feed(&s, 3601, 100));
    CHECK(!feed(&s, 10, 0));            // valve closed, water stopped
    CHECK(!feed(&s, 3600, 100));        // opened again in the app
    CHECK(feed(&s, 1, 100));
}

static void reopened_before_water_stopped_is_guarded_again(void)
{
    tier0_state_t s;
    tier0_init(&s);
    t = 0;
    CHECK(feed(&s, 3601, 100));         // trip
    CHECK(!feed(&s, 5, 100));           // valve still closing, water running
    tier0_valve_opened(&s, t);          // opened again before the flow ended
    CHECK(!feed(&s, 3599, 100));
    CHECK(feed(&s, 1, 100));            // a full hour after the reopen: trips again
}

static void without_reopen_a_running_flow_does_not_trip_twice(void)
{
    tier0_state_t s;
    tier0_init(&s);
    t = 0;
    CHECK(feed(&s, 3601, 100));
    CHECK(!feed(&s, 7200, 100));        // valve failed to close: the alert handles it, no repeated trips
}

int main(void)
{
    RUN(limit_is_one_hour);
    RUN(trips_after_an_hour_once);
    RUN(trickle_with_short_pauses_is_continuous);
    RUN(pause_longer_than_gap_restarts_the_hour);
    RUN(water_turned_on_again_after_a_trip_gets_a_new_hour);
    RUN(reopened_before_water_stopped_is_guarded_again);
    RUN(without_reopen_a_running_flow_does_not_trip_twice);
    return report();
}
