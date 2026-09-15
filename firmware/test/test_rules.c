#include "rules.h"
#include "unity_lite.h"

#define K 477

static rules_config_t base_config(void)
{
    return (rules_config_t) {.pulses_per_liter = K};
}

/* seconds of flow at lpm, 1 s samples */
static rules_decision_t flow(rules_state_t *s, const rules_config_t *c, int64_t *t, int seconds, uint32_t lpm, int hour)
{
    rules_decision_t last = {0};
    for (int i = 0; i < seconds; i++) {
        *t += 1000;
        rules_sample_t smp = {.now_ms = *t, .local_hour = hour, .pulses = lpm * K / 60, .sample_ms = 1000,
                              .flowing = lpm > 0};
        rules_decision_t d = rules_update(s, c, &smp);
        if (d.close || d.notify) return d;
        last = d;
    }
    return last;
}

static void all_rules_off_never_trigger(void)
{
    rules_state_t s;
    rules_init(&s);
    rules_config_t c = base_config();
    int64_t t = 0;
    rules_decision_t d = flow(&s, &c, &t, 3600, 40, 3);
    CHECK(!d.close);
    CHECK(!d.notify);
}

static void max_volume_closes_once(void)
{
    rules_state_t s;
    rules_init(&s);
    rules_config_t c = base_config();
    c.max_event_liters = 100;
    int64_t t = 0;
    rules_decision_t d = flow(&s, &c, &t, 600, 20, 12);     // 20 L/min: 100 L after 300 s
    CHECK(d.close);
    CHECK_EQ(d.rule, RULE_MAX_VOLUME);
    CHECK(t > 300000 && t <= 304000);
    d = flow(&s, &c, &t, 30, 20, 12);                      // still flowing while the valve closes
    CHECK(!d.close);
}

static void volume_limit_is_not_rounded_to_whole_liters(void)
{
    rules_state_t s;
    rules_init(&s);
    rules_config_t c = base_config();
    c.max_event_liters = 5;
    int64_t t = 0;
    rules_sample_t smp = {.local_hour = 12, .sample_ms = 1000, .flowing = true};
    smp.now_ms = t += 1000;
    smp.pulses = 5 * K;
    CHECK(!rules_update(&s, &c, &smp).close);           // exactly 5.0 L
    smp.now_ms = t += 1000;
    smp.pulses = 1;
    CHECK(rules_update(&s, &c, &smp).close);            // 5.0 L + 1 pulse
}

static void volume_resets_between_flows(void)
{
    rules_state_t s;
    rules_init(&s);
    rules_config_t c = base_config();
    c.max_event_liters = 100;
    int64_t t = 0;
    for (int i = 0; i < 5; i++) {
        CHECK(!flow(&s, &c, &t, 240, 20, 12).close);        // 80 L each
        flow(&s, &c, &t, 5, 0, 12);                         // pause
    }
}

static void burst_needs_duration(void)
{
    rules_state_t s;
    rules_init(&s);
    rules_config_t c = base_config();
    c.burst_lpm = 35;
    c.burst_s = 30;
    int64_t t = 0;
    CHECK(!flow(&s, &c, &t, 20, 45, 12).close);
    CHECK(!flow(&s, &c, &t, 20, 20, 12).close);             // drops below: timer resets
    rules_decision_t d = flow(&s, &c, &t, 40, 45, 12);
    CHECK(d.close);
    CHECK_EQ(d.rule, RULE_BURST);
}

static void night_window_wraps_midnight(void)
{
    rules_state_t s;
    rules_init(&s);
    rules_config_t c = base_config();
    c.night_start_h = 23;
    c.night_end_h = 5;
    c.night_max_liters = 20;
    int64_t t = 0;
    CHECK(!flow(&s, &c, &t, 600, 10, 14).close);            // 100 L in the afternoon
    flow(&s, &c, &t, 5, 0, 14);
    rules_decision_t d = flow(&s, &c, &t, 600, 10, 2);      // 2 am
    CHECK(d.close);
    CHECK_EQ(d.rule, RULE_NIGHT);
}

static void night_rule_off_without_clock(void)
{
    rules_state_t s;
    rules_init(&s);
    rules_config_t c = base_config();
    c.night_start_h = 0;
    c.night_end_h = 5;
    c.night_max_liters = 5;
    int64_t t = 0;
    CHECK(!flow(&s, &c, &t, 600, 10, -1).close);
}

static void snooze_mutes_but_vacation_stays(void)
{
    rules_state_t s;
    rules_init(&s);
    rules_config_t c = base_config();
    c.max_event_liters = 50;
    int64_t t = 0;
    rules_snooze(&s, t, 60);
    CHECK(!flow(&s, &c, &t, 1200, 10, 12).close);           // 200 L while snoozed
    flow(&s, &c, &t, 5, 0, 12);
    c.vacation = true;
    c.vacation_max_liters = 5;
    rules_decision_t d = flow(&s, &c, &t, 600, 10, 12);
    CHECK(d.close);
    CHECK_EQ(d.rule, RULE_VACATION);
}

static void snooze_expires(void)
{
    rules_state_t s;
    rules_init(&s);
    rules_config_t c = base_config();
    c.max_event_liters = 50;
    int64_t t = 0;
    rules_snooze(&s, t, 1);
    CHECK_EQ(rules_snooze_left_s(&s, t), 60);
    rules_decision_t d = flow(&s, &c, &t, 600, 10, 12);     // 50 L reached after 5 min, snooze over after 1
    CHECK(d.close);
    CHECK_EQ(rules_snooze_left_s(&s, t), 0);
}

static void micro_leak_notifies_once(void)
{
    rules_state_t s;
    rules_init(&s);
    rules_config_t c = base_config();
    c.leak_notify_min = 120;
    int64_t t = 0;
    int notifications = 0;
    // A drip: 3 pulses every 10 s, each drip seen by Tier 1 as a short flow
    for (int i = 0; i < 6 * 60 * 3; i++) {                 // 3 h
        t += 10000;
        rules_sample_t smp = {.now_ms = t, .local_hour = 12, .pulses = 3, .sample_ms = 1000, .flowing = false};
        rules_decision_t d = rules_update(&s, &c, &smp);
        if (d.notify) {
            notifications++;
            CHECK_EQ(d.rule, RULE_LEAK);
            CHECK(!d.close);
        }
    }
    CHECK_EQ(notifications, 1);
}

static void leak_counter_resets_after_quiet_minute(void)
{
    rules_state_t s;
    rules_init(&s);
    rules_config_t c = base_config();
    c.leak_notify_min = 30;
    int64_t t = 0;
    for (int i = 0; i < 29 * 60; i++) {
        rules_sample_t smp = {.now_ms = t += 1000, .local_hour = 12, .pulses = 1, .sample_ms = 1000, .flowing = true};
        rules_update(&s, &c, &smp);
    }
    for (int i = 0; i < 120; i++) {
        rules_sample_t smp = {.now_ms = t += 1000, .local_hour = 12, .pulses = 0, .sample_ms = 1000, .flowing = false};
        CHECK(!rules_update(&s, &c, &smp).notify);
    }
    CHECK_EQ(s.active_minutes, 0);
}

int main(void)
{
    RUN(all_rules_off_never_trigger);
    RUN(max_volume_closes_once);
    RUN(volume_limit_is_not_rounded_to_whole_liters);
    RUN(volume_resets_between_flows);
    RUN(burst_needs_duration);
    RUN(night_window_wraps_midnight);
    RUN(night_rule_off_without_clock);
    RUN(snooze_mutes_but_vacation_stays);
    RUN(snooze_expires);
    RUN(micro_leak_notifies_once);
    RUN(leak_counter_resets_after_quiet_minute);
    return report();
}
