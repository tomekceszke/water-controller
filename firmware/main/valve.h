#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    VALVE_OPEN = 0,
    VALVE_CLOSED = 1,
} valve_state_t;

typedef enum {
    VALVE_BY_RESTORE = 0,   // state restored at boot
    VALVE_BY_USER,          // web app
    VALVE_BY_ADMIN,         // script / API
    VALVE_BY_TIER1,         // continuous flow limit (set in the app)
    VALVE_BY_TIER2,         // anomaly rule
    VALVE_BY_TIER0,         // hard-coded 60 min ceiling
} valve_reason_t;

/* First thing at boot, before anything that can block or fail: drives the valve line to the state stored in NVS
 * (the state it had before the reset), so a reboot or power cut never changes it. Requires NVS. */
void valve_restore(void);

/* Drives the valve immediately, then persists state and reason. Safe from any task; idempotent. */
void valve_set(valve_state_t state, valve_reason_t reason, const char *detail);

valve_state_t valve_get(void);

typedef struct {
    valve_state_t state;
    valve_reason_t reason;
    char detail[24];
    int64_t changed_mono_ms;    // esp_timer time of the last change (0 = before this boot)
    int64_t changed_unix_s;     // wall clock of the last change, 0 = unknown
} valve_info_t;

void valve_info(valve_info_t *out);

const char *valve_reason_name(valve_reason_t reason);
