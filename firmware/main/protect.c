#include <stdio.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "hi_ntp.h"

#include "config/config.h"
#include "events.h"
#include "protect.h"
#include "rules.h"
#include "settings.h"
#include "valve.h"

static const char *TAG = "TIER2";

static QueueHandle_t s_queue;
static TaskHandle_t s_task;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static rules_state_t s_rules;
static volatile uint32_t s_dropped;
static rule_t s_last_rule = RULE_NONE;
static int64_t s_last_rule_ms;
static bool s_last_rule_closed;

static void rules_config_from(const settings_t *s, rules_config_t *c)
{
    *c = (rules_config_t) {
        .pulses_per_liter = s->pulses_per_liter,
        .max_event_liters = s->max_event_liters,
        .burst_lpm = s->burst_lpm,
        .burst_s = s->burst_s,
        .night_start_h = s->night_start_h,
        .night_end_h = s->night_end_h,
        .night_max_liters = s->night_max_liters,
        .leak_notify_min = s->leak_notify_min,
        .vacation = s->vacation,
        .vacation_max_liters = s->vacation_max_liters,
    };
}

static int local_hour(void)
{
    if (!hi_ntp_synced()) return -1;
    time_t now = time(NULL);
    struct tm t;
    localtime_r(&now, &t);
    return t.tm_hour;
}

static void protect_task(void *arg)
{
    protect_sample_t sample;
    for (;;) {
        if (xQueueReceive(s_queue, &sample, portMAX_DELAY) != pdTRUE) continue;

        settings_t s;
        settings_get(&s);
        rules_config_t cfg;
        rules_config_from(&s, &cfg);
        rules_sample_t in = {
            .now_ms = sample.now_ms,
            .local_hour = local_hour(),
            .pulses = sample.pulses,
            .sample_ms = sample.sample_ms,
            .flowing = sample.flowing,
        };

        portENTER_CRITICAL(&s_mux);
        rules_decision_t d = rules_update(&s_rules, &cfg, &in);
        portEXIT_CRITICAL(&s_mux);

        if (!d.close && !d.notify) continue;
        if (d.close && valve_get() == VALVE_CLOSED) continue;   // already shut: nothing to add

        s_last_rule = d.rule;
        s_last_rule_ms = sample.now_ms;
        s_last_rule_closed = d.close;
        event_t e = {
            .type = EV_RULE,
            .mono_ms = sample.now_ms,
            .reason = (uint8_t) d.rule,
            .closed = d.close,
            .pulses = (uint32_t) (d.liters * s.pulses_per_liter),
        };
        snprintf(e.detail, sizeof(e.detail), "%lu L, %lu L/min", (unsigned long) d.liters, (unsigned long) d.lpm);
        if (d.close) {
            char detail[24];
            snprintf(detail, sizeof(detail), "%s %lu L", rules_name(d.rule), (unsigned long) d.liters);
            ESP_LOGE(TAG, "Rule %s: closing the valve (%s)", rules_name(d.rule), e.detail);
            valve_set(VALVE_CLOSED, VALVE_BY_TIER2, detail);
        } else {
            ESP_LOGW(TAG, "Rule %s: notification (%s)", rules_name(d.rule), e.detail);
        }
        events_publish(&e);
    }
}

void protect_start(void)
{
    rules_init(&s_rules);
    s_queue = xQueueCreate(PROTECT_QUEUE_LEN, sizeof(protect_sample_t));
    if (s_queue == NULL
        || xTaskCreate(protect_task, "tier2", 4096, NULL, PROTECT_TASK_PRIORITY, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "Tier 2 not started (Tier 1 unaffected)");
        s_queue = NULL;
    }
}

void protect_post_sample(const protect_sample_t *sample)
{
    if (s_queue == NULL) return;
    if (xQueueSend(s_queue, sample, 0) != pdTRUE) s_dropped++;
}

void protect_snooze(uint32_t minutes)
{
    if (minutes > TIER2_MAX_SNOOZE_MIN) minutes = TIER2_MAX_SNOOZE_MIN;
    int64_t now_ms = esp_timer_get_time() / 1000;
    portENTER_CRITICAL(&s_mux);
    rules_snooze(&s_rules, now_ms, minutes);
    portEXIT_CRITICAL(&s_mux);
    ESP_LOGW(TAG, "(not error) Tier 2 snoozed for %lu min", (unsigned long) minutes);
}

void protect_status(protect_status_t *out)
{
    int64_t now_ms = esp_timer_get_time() / 1000;
    portENTER_CRITICAL(&s_mux);
    out->snooze_left_s = rules_snooze_left_s(&s_rules, now_ms);
    portEXIT_CRITICAL(&s_mux);
    out->dropped_samples = s_dropped;
    out->last_rule = rules_name(s_last_rule);
    out->last_rule_mono_ms = s_last_rule_ms;
    out->last_rule_closed = s_last_rule_closed;
}

#ifdef WATER_TEST_PULSES
void protect_test_suspend(bool suspend)
{
    if (s_task == NULL) return;
    if (suspend) {
        vTaskSuspend(s_task);
    } else {
        vTaskResume(s_task);
    }
}
#endif
