#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LEARNED_JSON_MAX 4096               // retained water/<mac>/config is ~3 KB

typedef struct {
    int64_t now_ms;
    uint32_t pulses;
    uint32_t sample_ms;
    bool flowing;
} protect_sample_t;

typedef struct {
    uint32_t snooze_left_s;
    uint32_t dropped_samples;
    const char *last_rule;          // "none" before the first trigger
    int64_t last_rule_mono_ms;
    bool last_rule_closed;
} protect_status_t;

/* Tier 2a task (local rules). Optional by design: if it lags or stops, Tier 1 is unaffected. */
void protect_start(void);

/* Non-blocking (called by Tier 1); samples are dropped when the queue is full. */
void protect_post_sample(const protect_sample_t *sample);

/* Mutes volume, burst and night rules; vacation stays armed. 0 cancels. */
void protect_snooze(uint32_t minutes);

void protect_status(protect_status_t *out);

typedef struct {
    bool active;                    // a valid config from hc-data is loaded
    char generated[11];             // its date, YYYY-MM-DD
    bool notify;                    // owner switch (Settings)
    bool flowing;
    uint32_t limit_s, limit_l;      // for the current flow (its start hour), else for the hour now; 0 = none
    uint32_t night_s, night_l;      // the coming night, 1:00-5:59: loosest of its hours; 0 = none
    uint32_t night_flows, night_limit;
} protect_learned_t;

typedef struct {
    int hour_now;
    uint32_t exp_l_x10[24], p90_l_x10[24], used_l_x10[24];     // today, local hours
} protect_learned_day_t;

/* Called from the MQTT task: copies the message and returns (never blocks, never parses). */
void protect_learned_offer(const char *data, size_t len);

void protect_learned_set_notify(bool on);

void protect_learned_status(protect_learned_t *out);

/* Today's expected and used liters per hour; false without learned limits or a synced clock. */
bool protect_learned_today(protect_learned_day_t *out, uint32_t pulses_per_liter);

#ifdef WATER_TEST_PULSES
/* Test builds only: simulates a hung Tier 2 task. */
void protect_test_suspend(bool suspend);
#endif
