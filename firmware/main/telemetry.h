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

/* Starts the shared MQTT client (hi_mqtt) and the telemetry task. Nothing here can block or stop
 * protection: producers only do a non-blocking queue send, the task talks to MQTT.
 *
 * Call after api_start(): the retained state topic is built from the same status document the API
 * serves. */
void telemetry_start(const char *mqtt_pass);

/* Non-blocking; dropped when the queue is full or telemetry is disabled. */
void telemetry_post_event(const event_t *event);
void telemetry_post_sample(const telemetry_sample_t *sample);

/* Items lost because the handover queue was full, as opposed to the MQTT outbox rejections that
 * hi_mqtt_stats() counts. */
uint32_t telemetry_dropped(void);
