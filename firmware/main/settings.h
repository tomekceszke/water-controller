#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    uint32_t tier1_limit_s;
    uint32_t pulses_per_liter;
    uint32_t max_event_liters;      // Tier 2, 0 = off
    uint32_t burst_lpm;
    uint32_t burst_s;               // 0 = off
    uint8_t night_start_h;
    uint8_t night_end_h;
    uint32_t night_max_liters;      // 0 = off
    uint32_t leak_notify_min;       // 0 = off
    bool vacation;
    uint32_t vacation_max_liters;
} settings_t;

/* Loads from NVS (defaults when missing or invalid). Never fails: protection must start either way. */
void settings_init(void);

/* Consistent copy (mutex-free; safe from any task). */
void settings_get(settings_t *out);

/* Tier 1 limit without copying the whole struct (atomic read). */
uint32_t settings_tier1_limit_s(void);

/* Clamps every field to its bounds, applies it immediately and persists it. The clamped value is written back. */
esp_err_t settings_set(settings_t *in_out);
