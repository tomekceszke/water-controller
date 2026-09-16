#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    EV_FLOW_END = 0,    // a continuous flow finished
    EV_VALVE,           // valve state changed
    EV_RULE,            // Tier 2 rule fired (close or notification)
    EV_ALERT,           // e.g. water still flowing after a close
} event_type_t;

typedef struct {
    event_type_t type;
    int64_t mono_ms;        // esp_timer time of the event (end of flow, change, trigger)
    uint32_t duration_ms;   // EV_FLOW_END
    uint32_t pulses;        // EV_FLOW_END, EV_RULE (flow so far)
    uint16_t max_lpm_x10;   // EV_FLOW_END
    uint8_t valve_state;    // EV_VALVE
    uint8_t reason;         // EV_VALVE: valve_reason_t, EV_RULE: rule_t
    bool closed;            // EV_FLOW_END: flow ended by a close; EV_RULE: close requested
    char detail[24];
} event_t;

typedef struct {
    uint64_t pulses_since_boot;
    uint64_t pulses_today;      // local day, counted from boot until the clock is synced
    uint32_t flows_since_boot;
    uint32_t small_flows_today;     // below FLOW_EVENT_MIN_ML: not listed, not sent as flow events
    uint32_t small_flows_since_boot;
} events_totals_t;

void events_init(void);

/* Never blocks: stores the event in the RAM ring, forwards it to telemetry and sends notifications for
 * closes and alerts. Callable from the Tier 1 task. */
void events_publish(const event_t *event);

/* Newest first; returns the number copied. */
size_t events_recent(event_t *out, size_t max);

void events_totals(events_totals_t *out);

/* Wall-clock time (s) of an esp_timer timestamp, 0 when the clock is not synced. */
int64_t events_unix_time(int64_t mono_ms);
