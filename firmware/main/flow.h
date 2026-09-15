#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool flowing;
    uint32_t lpm_x10;               // rate in the last sample, 0.1 L/min
    uint32_t flow_pulses;           // current (or last) continuous flow
    uint32_t flow_ms;
    uint32_t flow_max_lpm_x10;
    uint32_t tier1_limit_s;
    uint32_t tier1_remaining_s;     // 0 when not flowing
    uint64_t counter;               // pulses since boot (PCNT, accumulated)
    int64_t heartbeat_ms;           // last Tier 1 loop (esp_timer)
    bool diag_running;
    uint32_t diag_left_s;
    uint32_t diag_edges;            // raw rising edges seen by the GPIO interrupt
    uint32_t diag_short;            // edge intervals below 1 ms (faster than any real flow: noise)
    uint32_t diag_min_interval_us;  // shortest interval between rising edges
} flow_status_t;

/* Tier 1: configures the PCNT unit and starts the flow task (core FLOW_TASK_CORE, task watchdog).
 * Call right after valve_restore() and settings_init(). Reboots if the task cannot start. */
void flow_start(void);

void flow_status(flow_status_t *out);

/* True when the Tier 1 loop ran within the last few seconds (health predicate). */
bool flow_alive(void);

/* Pulse-interval diagnostics through a GPIO interrupt on the meter input for minutes (0 stops). */
void flow_diag(uint32_t minutes);
