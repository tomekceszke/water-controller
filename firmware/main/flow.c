#include <limits.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/pulse_cnt.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"

#include "config/config.h"
#include "events.h"
#include "flow.h"
#include "protect.h"
#include "settings.h"
#include "telemetry.h"
#include "tier0.h"
#include "tier1.h"
#include "valve.h"

/*
 * Tier 1 protection. Deliberately self-contained: no network, no allocation in the loop, never waits on a queue.
 * The only shared resources it touches are the valve (GPIO + a short NVS write) and non-blocking queue sends.
 */

static const char *TAG = "FLOW";

static pcnt_unit_handle_t s_pcnt;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static flow_status_t s_status;

/* ---------- diagnostics (GPIO interrupt on the meter input) ---------- */

static volatile int64_t s_diag_until_ms;
static volatile uint32_t s_diag_edges;
static volatile uint32_t s_diag_short;
static volatile uint32_t s_diag_min_us = UINT32_MAX;
static volatile int64_t s_diag_last_us;
static bool s_isr_installed;

static void IRAM_ATTR diag_isr(void *arg)
{
    int64_t now = esp_timer_get_time();
    if (s_diag_last_us != 0) {
        int64_t interval = now - s_diag_last_us;
        if (interval < s_diag_min_us) s_diag_min_us = (uint32_t) interval;
        if (interval < 1000) s_diag_short++;
    }
    s_diag_last_us = now;
    s_diag_edges++;
}

static void diag_enable(bool enable)
{
    if (enable) {
        if (!s_isr_installed) {
            gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
            gpio_isr_handler_add(GPIO_FLOW_METER, diag_isr, NULL);
            s_isr_installed = true;
        }
        s_diag_edges = 0;
        s_diag_short = 0;
        s_diag_min_us = UINT32_MAX;
        s_diag_last_us = 0;
        gpio_set_intr_type(GPIO_FLOW_METER, GPIO_INTR_POSEDGE);
        gpio_intr_enable(GPIO_FLOW_METER);
    } else if (s_isr_installed) {
        gpio_intr_disable(GPIO_FLOW_METER);
    }
}

void flow_diag(uint32_t minutes)
{
    if (minutes > FLOW_DIAG_MAX_MIN) minutes = FLOW_DIAG_MAX_MIN;
    int64_t now_ms = esp_timer_get_time() / 1000;
    s_diag_until_ms = minutes ? now_ms + (int64_t) minutes * 60000 : 0;
    diag_enable(minutes > 0);
    ESP_LOGI(TAG, "Pulse diagnostics %s", minutes ? "started" : "stopped");
}

/* ---------- PCNT ---------- */

static void pcnt_init(void)
{
    pcnt_unit_config_t unit_config = {
        .high_limit = SHRT_MAX,
        .low_limit = -1,
        .flags.accum_count = 1,     // keep counting across the 16-bit hardware limit
    };
    ESP_ERROR_CHECK(pcnt_new_unit(&unit_config, &s_pcnt));
    ESP_ERROR_CHECK(pcnt_unit_set_glitch_filter(s_pcnt, &(pcnt_glitch_filter_config_t) {.max_glitch_ns = FLOW_GLITCH_NS}));
    pcnt_chan_config_t chan_config = {
        .edge_gpio_num = GPIO_FLOW_METER,   // the driver enables the internal pull-up (open-collector meter)
        .level_gpio_num = -1,
    };
    pcnt_channel_handle_t chan = NULL;
    ESP_ERROR_CHECK(pcnt_new_channel(s_pcnt, &chan_config, &chan));
    ESP_ERROR_CHECK(pcnt_channel_set_edge_action(chan, PCNT_CHANNEL_EDGE_ACTION_INCREASE,
                                                 PCNT_CHANNEL_EDGE_ACTION_HOLD));
    ESP_ERROR_CHECK(pcnt_unit_add_watch_point(s_pcnt, SHRT_MAX));   // required for accumulation
    ESP_ERROR_CHECK(pcnt_unit_enable(s_pcnt));
    ESP_ERROR_CHECK(pcnt_unit_clear_count(s_pcnt));
    ESP_ERROR_CHECK(pcnt_unit_start(s_pcnt));
}

/* ---------- Tier 1 loop ---------- */

static void publish_flow_end(const tier1_state_t *t1, int64_t now_ms, uint32_t max_lpm_x10)
{
    event_t e = {
        .type = EV_FLOW_END,
        .mono_ms = t1->last_pulse_ms,
        .duration_ms = (uint32_t) tier1_elapsed_ms(t1, now_ms),
        .pulses = t1->pulses > UINT32_MAX ? UINT32_MAX : (uint32_t) t1->pulses,
        .max_lpm_x10 = max_lpm_x10 > UINT16_MAX ? UINT16_MAX : (uint16_t) max_lpm_x10,
        .closed = valve_get() == VALVE_CLOSED,
    };
    events_publish(&e);
}

static void flow_task(void *arg)
{
    esp_task_wdt_add(NULL);

    tier0_state_t t0;
    tier0_init(&t0);
    tier1_state_t t1;
    tier1_init(&t1);
    int prev_count = 0;
    uint32_t max_lpm_x10 = 0;
    telemetry_sample_t tele = {0};
    int64_t closed_flow_since_ms = 0;       // water flowing while the valve is closed
    valve_state_t last_valve = valve_get();
    bool alert_sent = false;
    TickType_t wake = xTaskGetTickCount();
    int64_t last_ms = esp_timer_get_time() / 1000;

    for (;;) {
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(FLOW_SAMPLE_MS));
        int64_t now_ms = esp_timer_get_time() / 1000;
        uint32_t dt_ms = (uint32_t) (now_ms - last_ms);
        last_ms = now_ms;

        int count = prev_count;
        if (pcnt_unit_get_count(s_pcnt, &count) != ESP_OK) count = prev_count;
        uint32_t pulses = (uint32_t) count - (uint32_t) prev_count;     // wraps correctly
        prev_count = count;

        uint32_t ppl = 0;
        {
            settings_t s;
            settings_get(&s);
            ppl = s.pulses_per_liter;
        }
        tier1_config_t cfg = {.limit_s = settings_tier1_limit_s(), .gap_ms = TIER1_GAP_MS};
        // A person opened the valve again: water that is still running gets a fresh limit, never none
        valve_state_t valve_now = valve_get();
        if (last_valve == VALVE_CLOSED && valve_now == VALVE_OPEN) {
            tier0_valve_opened(&t0, now_ms);
            tier1_valve_opened(&t1, now_ms);
        }
        last_valve = valve_now;
        bool was_flowing = t1.flowing;
        // Tier 0 first and independent of settings: nothing configurable can raise or disable it
        bool t0_trip = tier0_update(&t0, now_ms, pulses);
        tier1_result_t r = tier1_update(&t1, &cfg, now_ms, pulses);

        uint32_t lpm_x10 = dt_ms && ppl ? (uint32_t) ((uint64_t) pulses * 600000u / ((uint64_t) dt_ms * ppl)) : 0;
        if (r == TIER1_STARTED || (!was_flowing && t1.flowing)) max_lpm_x10 = 0;
        if (t1.flowing && lpm_x10 > max_lpm_x10) max_lpm_x10 = lpm_x10;

        if (t0_trip) {
            ESP_LOGE(TAG, "Tier 0: continuous flow for %d min, closing the valve", TIER0_LIMIT_S / 60);
            char detail[24];
            snprintf(detail, sizeof(detail), "flow > %d min", TIER0_LIMIT_S / 60);
            valve_set(VALVE_CLOSED, VALVE_BY_TIER0, detail);
        }
        if (r == TIER1_TRIP) {
            char detail[24];
            snprintf(detail, sizeof(detail), "flow > %lu min", (unsigned long) (cfg.limit_s / 60));
            ESP_LOGE(TAG, "Tier 1: continuous flow for %lu s, closing the valve", (unsigned long) cfg.limit_s);
            valve_set(VALVE_CLOSED, VALVE_BY_TIER1, detail);
        } else if (r == TIER1_ENDED) {
            publish_flow_end(&t1, now_ms, max_lpm_x10);
        }

        // Valve closed (by anyone) but water keeps flowing: actuator failure or a leak before the valve
        if (valve_get() == VALVE_CLOSED && t1.flowing) {
            if (closed_flow_since_ms == 0) closed_flow_since_ms = now_ms;
            if (!alert_sent && now_ms - closed_flow_since_ms > VALVE_CLOSING_S * 1000) {
                event_t e = {.type = EV_ALERT, .mono_ms = now_ms};
                snprintf(e.detail, sizeof(e.detail), "%u.%u L/min",
                         (unsigned) ((lpm_x10 / 10) % 1000), (unsigned) (lpm_x10 % 10));
                events_publish(&e);
                alert_sent = true;
            }
        } else {
            closed_flow_since_ms = 0;
            if (!t1.flowing) alert_sent = false;
        }

        gpio_set_level(GPIO_LED_FLOW, t1.flowing);
        protect_post_sample(&(protect_sample_t) {
            .now_ms = now_ms, .pulses = pulses, .sample_ms = dt_ms, .flowing = t1.flowing,
        });

        if (t1.flowing || tele.pulses) {
            if (tele.period_ms == 0) tele.mono_ms = now_ms;
            tele.pulses += pulses;
            tele.period_ms += dt_ms;
            if (tele.period_ms >= TELEMETRY_SAMPLE_PERIOD_S * 1000 || !t1.flowing) {
                tele.mono_ms = now_ms;
                tele.flow_pulses = (uint32_t) t1.pulses;
                telemetry_post_sample(&tele);
                tele = (telemetry_sample_t) {0};
            }
        }

        int64_t diag_until = s_diag_until_ms;
        if (diag_until != 0 && now_ms >= diag_until) {
            s_diag_until_ms = 0;
            diag_enable(false);
        }

        portENTER_CRITICAL(&s_mux);
        s_status.flowing = t1.flowing;
        s_status.lpm_x10 = lpm_x10;
        s_status.flow_pulses = t1.pulses > UINT32_MAX ? UINT32_MAX : (uint32_t) t1.pulses;
        s_status.flow_ms = (uint32_t) tier1_elapsed_ms(&t1, now_ms);
        s_status.flow_max_lpm_x10 = max_lpm_x10;
        s_status.tier1_limit_s = cfg.limit_s;
        s_status.tier1_remaining_s = tier1_remaining_s(&t1, &cfg, now_ms);
        s_status.counter = (uint64_t) (uint32_t) count;
        s_status.heartbeat_ms = now_ms;
        s_status.diag_running = s_diag_until_ms != 0;
        s_status.diag_left_s = s_diag_until_ms ? (uint32_t) ((s_diag_until_ms - now_ms) / 1000) : 0;
        s_status.diag_edges = s_diag_edges;
        s_status.diag_short = s_diag_short;
        s_status.diag_min_interval_us = s_diag_min_us;
        portEXIT_CRITICAL(&s_mux);

        esp_task_wdt_reset();
    }
}

void flow_start(void)
{
    pcnt_init();
    s_status.tier1_limit_s = settings_tier1_limit_s();
    s_status.heartbeat_ms = esp_timer_get_time() / 1000;
    if (xTaskCreatePinnedToCore(flow_task, "tier1", 6144, NULL, FLOW_TASK_PRIORITY, NULL, FLOW_TASK_CORE) != pdPASS) {
        ESP_LOGE(TAG, "Tier 1 task not created, rebooting");
        esp_restart();
    }
    ESP_LOGI(TAG, "Tier 1 running: limit %lu s", (unsigned long) settings_tier1_limit_s());
}

void flow_status(flow_status_t *out)
{
    portENTER_CRITICAL(&s_mux);
    *out = s_status;
    portEXIT_CRITICAL(&s_mux);
}

bool flow_alive(void)
{
    flow_status_t s;
    flow_status(&s);
    return esp_timer_get_time() / 1000 - s.heartbeat_ms < 5 * FLOW_SAMPLE_MS;
}
