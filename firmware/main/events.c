#include <stdio.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "esp_timer.h"

#include "hi_notify.h"
#include "hi_ntp.h"

#include "config/config.h"
#include "events.h"
#include "rules.h"
#include "settings.h"
#include "telemetry.h"
#include "valve.h"

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static event_t s_ring[EVENTS_RING_SIZE];
static size_t s_head;       // next write position
static size_t s_count;
static events_totals_t s_totals;
static int s_today_yday = -1;

void events_init(void)
{
    s_head = 0;
    s_count = 0;
    memset(&s_totals, 0, sizeof(s_totals));
}

int64_t events_unix_time(int64_t mono_ms)
{
    if (!hi_ntp_synced()) return 0;
    int64_t now_mono_ms = esp_timer_get_time() / 1000;
    return (int64_t) time(NULL) - (now_mono_ms - mono_ms) / 1000;
}

static void update_totals(const event_t *e)
{
    if (e->type != EV_FLOW_END) return;
    int yday = -1;
    if (hi_ntp_synced()) {
        time_t now = time(NULL);
        struct tm t;
        localtime_r(&now, &t);
        yday = t.tm_yday;
    }
    portENTER_CRITICAL(&s_mux);
    if (yday >= 0 && yday != s_today_yday) {
        if (s_today_yday >= 0) s_totals.pulses_today = 0;     // keep pre-sync flows in the first synced day
        s_today_yday = yday;
    }
    s_totals.pulses_since_boot += e->pulses;
    s_totals.pulses_today += e->pulses;
    s_totals.flows_since_boot++;
    portEXIT_CRITICAL(&s_mux);
}

static void notify(const event_t *e)
{
    char title[48];
    char msg[160];
    settings_t s;
    switch (e->type) {
        case EV_VALVE:
            if (e->valve_state == VALVE_CLOSED) {
                snprintf(title, sizeof(title), "Water shut off");
                snprintf(msg, sizeof(msg), "Valve closed by %s%s%s", valve_reason_name((valve_reason_t) e->reason),
                         e->detail[0] ? ": " : "", e->detail);
                hi_notify_event_ex(title, msg, e->reason == VALVE_BY_USER ? HI_NOTIFY_PRIO_DEFAULT
                                                                          : HI_NOTIFY_PRIO_URGENT, "droplet,no_entry");
            } else {
                snprintf(msg, sizeof(msg), "Valve opened by %s", valve_reason_name((valve_reason_t) e->reason));
                hi_notify_event_ex("Water on", msg, HI_NOTIFY_PRIO_LOW, "droplet");
            }
            break;
        case EV_RULE:
            if (!e->closed) {
                settings_get(&s);
                snprintf(msg, sizeof(msg), "Water has been running continuously for %lu min (%s)",
                         (unsigned long) s.leak_notify_min, rules_name((rule_t) e->reason));
                hi_notify_event_ex("Possible leak", msg, HI_NOTIFY_PRIO_HIGH, "droplet,warning");
            }
            break;
        case EV_ALERT:
            hi_notify_event_ex("Water still flowing", e->detail, HI_NOTIFY_PRIO_URGENT, "rotating_light");
            break;
        default:
            break;
    }
}

void events_publish(const event_t *e)
{
    portENTER_CRITICAL(&s_mux);
    s_ring[s_head] = *e;
    s_head = (s_head + 1) % EVENTS_RING_SIZE;
    if (s_count < EVENTS_RING_SIZE) s_count++;
    portEXIT_CRITICAL(&s_mux);

    update_totals(e);
    telemetry_post_event(e);
    notify(e);
}

size_t events_recent(event_t *out, size_t max)
{
    size_t n = 0;
    portENTER_CRITICAL(&s_mux);
    for (size_t i = 0; i < s_count && n < max; i++) {
        size_t idx = (s_head + EVENTS_RING_SIZE - 1 - i) % EVENTS_RING_SIZE;
        out[n++] = s_ring[idx];
    }
    portEXIT_CRITICAL(&s_mux);
    return n;
}

void events_totals(events_totals_t *out)
{
    portENTER_CRITICAL(&s_mux);
    *out = s_totals;
    portEXIT_CRITICAL(&s_mux);
}
