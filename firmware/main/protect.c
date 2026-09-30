#include <stdio.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"

#include "hi_ntp.h"

#include "config/config.h"
#include "events.h"
#include "learned.h"
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

/* Tier 2b: learned limits. The MQTT task only copies an incoming config into s_rx and raises s_rx_pending; this
 * task parses it, persists it and swaps it in. Everything else here is owned by this task, except what
 * protect_learned_status()/protect_learned_today() copy out under s_mux. */
static const char *LEARNED_NVS_NAMESPACE = "wc_learned";
static char s_rx[LEARNED_JSON_MAX + 1];
static volatile bool s_rx_pending;
static learned_config_t s_learned;
static learned_state_t s_learned_state;
static bool s_learned_notify = true;
static uint32_t s_hour_pulses[24];      // today, per local hour
static int s_hour_yday = -1;
static int s_now_how = -1;              // local hour of week, for the status

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

/* Local time for the rules; all -1 while the clock is not synced. */
static void local_time(int *dow, int *hour, int *yday)
{
    *dow = *hour = *yday = -1;
    if (!hi_ntp_synced()) return;
    time_t now = time(NULL);
    struct tm t;
    localtime_r(&now, &t);
    *dow = (t.tm_wday + 6) % 7;         // Monday = 0
    *hour = t.tm_hour;
    *yday = t.tm_yday;
}

static void learned_load(void)
{
    nvs_handle_t h;
    if (nvs_open(LEARNED_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "No learned limits stored yet");
        return;
    }
    learned_config_t c;
    size_t len = sizeof(c);
    if (nvs_get_blob(h, "config", &c, &len) == ESP_OK && len == sizeof(c) && c.valid) {
        c.generated[sizeof(c.generated) - 1] = '\0';
        s_learned = c;
        ESP_LOGI(TAG, "Learned limits from %s", s_learned.generated);
    }
    uint8_t notify;
    if (nvs_get_u8(h, "notify", &notify) == ESP_OK) s_learned_notify = notify != 0;
    nvs_close(h);
}

static void learned_store(const char *key, const void *blob, size_t len, uint8_t u8)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(LEARNED_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = blob ? nvs_set_blob(h, key, blob, len) : nvs_set_u8(h, key, u8);
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    }
    if (err != ESP_OK) ESP_LOGW(TAG, "Learned %s not saved: %s", key, esp_err_to_name(err));
}

static void learned_apply_pending(void)
{
    if (!s_rx_pending) return;
    __sync_synchronize();
    static learned_config_t c;          // 1.4 KB: kept off the task stack
    const bool ok = learned_parse(s_rx, &c);
    s_rx_pending = false;
    if (!ok) {
        ESP_LOGW(TAG, "Learned limits rejected (bad format); keeping the previous ones");
        return;
    }
    if (s_learned.valid && memcmp(&c, &s_learned, sizeof(c)) == 0) return;   // same retained message again
    portENTER_CRITICAL(&s_mux);
    s_learned = c;
    portEXIT_CRITICAL(&s_mux);
    learned_store("config", &c, sizeof(c), 0);
    ESP_LOGI(TAG, "Learned limits from %s applied", c.generated);
}

static void learned_step(const protect_sample_t *sample, const settings_t *s, int dow, int hour, int yday,
                         bool snoozed)
{
    if (yday >= 0 && yday != s_hour_yday) {
        portENTER_CRITICAL(&s_mux);
        memset(s_hour_pulses, 0, sizeof(s_hour_pulses));
        s_hour_yday = yday;
        portEXIT_CRITICAL(&s_mux);
    }
    portENTER_CRITICAL(&s_mux);
    if (hour >= 0) s_hour_pulses[hour] += sample->pulses;
    s_now_how = learned_how(dow, hour);
    portEXIT_CRITICAL(&s_mux);

    const learned_sample_t in = {
        .now_ms = sample->now_ms, .pulses = sample->pulses, .flowing = sample->flowing,
        .local_dow = dow, .local_hour = hour, .local_yday = yday, .pulses_per_liter = s->pulses_per_liter,
        .max_dur_s = tier2_time_cap_s(s->tier1_limit_s), .max_vol_l = tier2_volume_cap_l(s->tier1_limit_s),
    };
    portENTER_CRITICAL(&s_mux);
    const learned_decision_t d = learned_update(&s_learned_state, &s_learned, &in);
    portEXIT_CRITICAL(&s_mux);
    if (d.hit == LEARNED_NONE || !s_learned_notify || snoozed) return;

    const rule_t rule = d.hit == LEARNED_DURATION ? RULE_LEARNED_DURATION
                      : d.hit == LEARNED_VOLUME ? RULE_LEARNED_VOLUME : RULE_NIGHT_FLOWS;
    s_last_rule = rule;
    s_last_rule_ms = sample->now_ms;
    s_last_rule_closed = false;
    event_t e = {
        .type = EV_RULE, .mono_ms = sample->now_ms, .reason = (uint8_t) rule, .closed = false,
        .pulses = (uint32_t) s_learned_state.pulses, .hour = d.hour, .value = d.value, .limit = d.limit,
    };
    snprintf(e.detail, sizeof(e.detail), "%lu > %lu", (unsigned long) d.value, (unsigned long) d.limit);
    ESP_LOGW(TAG, "(not error) Learned limit %s: %s", rules_name(rule), e.detail);
    events_publish(&e);
}

static void protect_task(void *arg)
{
    protect_sample_t sample;
    for (;;) {
        if (xQueueReceive(s_queue, &sample, portMAX_DELAY) != pdTRUE) continue;

        learned_apply_pending();
        settings_t s;
        settings_get(&s);
        rules_config_t cfg;
        rules_config_from(&s, &cfg);
        int dow, hour, yday;
        local_time(&dow, &hour, &yday);
        rules_sample_t in = {
            .now_ms = sample.now_ms,
            .local_hour = hour,
            .pulses = sample.pulses,
            .sample_ms = sample.sample_ms,
            .flowing = sample.flowing,
        };

        portENTER_CRITICAL(&s_mux);
        rules_decision_t d = rules_update(&s_rules, &cfg, &in);
        const bool snoozed = rules_snooze_left_s(&s_rules, sample.now_ms) > 0;
        portEXIT_CRITICAL(&s_mux);

        learned_step(&sample, &s, dow, hour, yday, snoozed);

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
    learned_init(&s_learned_state);
    learned_load();
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

void protect_learned_offer(const char *data, size_t len)
{
    if (len == 0 || len > LEARNED_JSON_MAX) return;     // a deleted retained config keeps the last limits
    if (s_rx_pending) return;                            // the previous one is not consumed yet: the next wins later
    memcpy(s_rx, data, len);
    s_rx[len] = '\0';
    __sync_synchronize();                               // the buffer is complete before the flag is seen
    s_rx_pending = true;
}

void protect_learned_set_notify(bool on)
{
    s_learned_notify = on;
    learned_store("notify", NULL, 0, on ? 1 : 0);
}

void protect_learned_status(protect_learned_t *out)
{
    memset(out, 0, sizeof(*out));
    portENTER_CRITICAL(&s_mux);
    out->active = s_learned.valid;
    memcpy(out->generated, s_learned.generated, sizeof(out->generated));
    out->notify = s_learned_notify;
    // The current flow is judged by the hour it started in; without a flow, the hour now
    const int how = s_learned_state.flowing && s_learned_state.how >= 0 ? s_learned_state.how : s_now_how;
    if (s_learned.valid && how >= 0) {
        out->limit_s = learned_dur_limit(&s_learned, how, tier2_time_cap_s(settings_tier1_limit_s()));
        out->limit_l = learned_vol_limit(&s_learned, how, tier2_volume_cap_l(settings_tier1_limit_s()));
    }
    out->flowing = s_learned_state.flowing;
    out->night_limit = s_learned.valid ? s_learned.night_flows : 0;
    out->night_flows = s_learned_state.night_yday == s_hour_yday ? s_learned_state.night_flows : 0;
    portEXIT_CRITICAL(&s_mux);
}

bool protect_learned_today(protect_learned_day_t *out, uint32_t pulses_per_liter)
{
    memset(out, 0, sizeof(*out));
    portENTER_CRITICAL(&s_mux);
    const bool ok = s_learned.valid && s_now_how >= 0;
    if (ok) {
        const int base = (s_now_how / 24) * 24;
        out->hour_now = s_now_how % 24;
        for (int h = 0; h < 24; h++) {
            out->exp_l_x10[h] = s_learned.exp_l_x10[base + h];
            out->p90_l_x10[h] = s_learned.p90_l_x10[base + h];
            out->used_l_x10[h] = pulses_per_liter ? (uint32_t) ((uint64_t) s_hour_pulses[h] * 10 / pulses_per_liter) : 0;
        }
    }
    portEXIT_CRITICAL(&s_mux);
    return ok;
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
