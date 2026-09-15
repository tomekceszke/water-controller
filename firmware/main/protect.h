#pragma once

#include <stdbool.h>
#include <stdint.h>

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
