#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "events.h"

typedef struct {
    int64_t mono_ms;
    uint32_t pulses;        // pulses in the period
    uint32_t period_ms;
    uint32_t flow_pulses;   // pulses of the running flow so far
} telemetry_sample_t;

typedef struct {
    bool enabled;
    bool connected;
    uint32_t dropped;       // items lost because the queue was full
    int outbox_bytes;
} telemetry_stats_t;

/* Starts the MQTT client and the telemetry task. Nothing here can block or stop protection:
 * producers only do a non-blocking queue send, the task talks to MQTT. */
void telemetry_start(void);

/* Non-blocking; dropped when the queue is full or telemetry is disabled. */
void telemetry_post_event(const event_t *event);
void telemetry_post_sample(const telemetry_sample_t *sample);

void telemetry_stats(telemetry_stats_t *out);
