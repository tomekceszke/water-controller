#include <inttypes.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "hi_mqtt.h"
#include "hi_ntp.h"

#include "api.h"

#include "config/config.h"
#include "rules.h"
#include "settings.h"
#include "telemetry.h"
#include "valve.h"

/*
 * Publishes to Mosquitto on hc-data through hi_mqtt (QoS 1, outbox bounded by MQTT_OUTBOX_LIMIT_BYTES):
 *   water/<mac>/flow     {"start":<unix>,"stop":<unix>,"pulses":n,"liters":x,"max_lpm":x,"closed":bool}
 *   water/<mac>/sample   {"ts":<unix>,"pulses":n,"period_ms":n,"lpm":x,"flow_pulses":n}
 *   water/<mac>/valve    {"ts":<unix>,"state":"open|closed","reason":"...","detail":"..."}
 *   water/<mac>/rule     {"ts":<unix>,"rule":"...","close":bool,"pulses":n,"detail":"..."}
 *   water/<mac>/alert    {"ts":<unix>,"detail":"..."}
 *   water/<mac>/status   "online" / "offline" (retained, LWT; published by hi_mqtt)
 *   water/<mac>/state    the full status JSON (retained; published by hi_mqtt)
 * Items created before the clock is synced wait in the queue and get their wall-clock time once it is.
 */

static const char *TAG = "TELEMETRY";

#define KIND_MAX_LEN 16
#define PAYLOAD_MAX_LEN 256
#define WAIT_FOR_CLOCK_MS 1000

typedef struct {
    bool is_sample;
    union {
        event_t event;
        telemetry_sample_t sample;
    };
} item_t;

static QueueHandle_t s_queue;
static volatile uint32_t s_dropped;

static int64_t unix_of(int64_t mono_ms)
{
    return events_unix_time(mono_ms);
}

static void publish(const char *kind, const char *payload)
{
    hi_mqtt_publish(kind, payload, 1, false);
}

static void publish_item(const item_t *item)
{
    char p[PAYLOAD_MAX_LEN];
    settings_t s;
    settings_get(&s);
    if (item->is_sample) {
        const telemetry_sample_t *m = &item->sample;
        double lpm = m->period_ms ? (double) m->pulses * 60000.0 / ((double) m->period_ms * s.pulses_per_liter) : 0;
        snprintf(p, sizeof(p), "{\"ts\":%" PRId64 ",\"pulses\":%" PRIu32 ",\"period_ms\":%" PRIu32
                 ",\"lpm\":%.2f,\"flow_pulses\":%" PRIu32 "}",
                 unix_of(m->mono_ms), m->pulses, m->period_ms, lpm, m->flow_pulses);
        publish("sample", p);
        return;
    }
    const event_t *e = &item->event;
    int64_t ts = unix_of(e->mono_ms);
    switch (e->type) {
        case EV_FLOW_END:
            snprintf(p, sizeof(p), "{\"start\":%" PRId64 ",\"stop\":%" PRId64 ",\"pulses\":%" PRIu32
                     ",\"liters\":%.3f,\"max_lpm\":%.1f,\"closed\":%s,\"pulses_per_liter\":%" PRIu32 "}",
                     unix_of(e->mono_ms - e->duration_ms), ts, e->pulses, (double) e->pulses / s.pulses_per_liter,
                     e->max_lpm_x10 / 10.0, e->closed ? "true" : "false", s.pulses_per_liter);
            publish("flow", p);
            break;
        case EV_VALVE:
            snprintf(p, sizeof(p), "{\"ts\":%" PRId64 ",\"state\":\"%s\",\"reason\":\"%s\",\"detail\":\"%s\"}", ts,
                     e->valve_state == VALVE_CLOSED ? "closed" : "open",
                     valve_reason_name((valve_reason_t) e->reason), e->detail);
            publish("valve", p);
            break;
        case EV_RULE:
            snprintf(p, sizeof(p), "{\"ts\":%" PRId64 ",\"rule\":\"%s\",\"close\":%s,\"pulses\":%" PRIu32
                     ",\"detail\":\"%s\"}", ts, rules_name((rule_t) e->reason), e->closed ? "true" : "false",
                     e->pulses, e->detail);
            publish("rule", p);
            break;
        case EV_ALERT:
            snprintf(p, sizeof(p), "{\"ts\":%" PRId64 ",\"detail\":\"%s\"}", ts, e->detail);
            publish("alert", p);
            break;
    }
}

static void telemetry_task(void *arg)
{
    item_t item;
    for (;;) {
        if (xQueuePeek(s_queue, &item, portMAX_DELAY) != pdTRUE) continue;
        // Without a wall clock the timestamps would be wrong: keep the item (the queue drops new ones when full)
        if (!hi_ntp_synced()) {
            vTaskDelay(pdMS_TO_TICKS(WAIT_FOR_CLOCK_MS));
            continue;
        }
        xQueueReceive(s_queue, &item, 0);
        publish_item(&item);
    }
}

void telemetry_start(const char *mqtt_pass)
{
    s_queue = xQueueCreate(TELEMETRY_QUEUE_LEN, sizeof(item_t));
    if (s_queue == NULL) {
        ESP_LOGW(TAG, "Telemetry disabled: out of memory");
        return;
    }
    const esp_err_t err = hi_mqtt_start(&(hi_mqtt_config_t) {
        .broker_uri = MQTT_BROKER_URI,
        .topic_prefix = MQTT_TOPIC_PREFIX,
        .username = TELEMETRY_MQTT_USER,
        .password = mqtt_pass,
        .client_id_prefix = "wc",
        .outbox_limit_bytes = MQTT_OUTBOX_LIMIT_BYTES,
        .state_fn = api_status_json,
        .state_period_s = MQTT_STATE_PERIOD_S,
    });
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Telemetry disabled: MQTT client not started (%s)", esp_err_to_name(err));
        vQueueDelete(s_queue);
        s_queue = NULL;
        return;
    }
    if (xTaskCreate(telemetry_task, "telemetry", 4096, NULL, 3, NULL) != pdPASS) {
        ESP_LOGW(TAG, "Telemetry task not started");
    }
}

static void post(const item_t *item)
{
    if (s_queue == NULL) return;
    if (xQueueSend(s_queue, item, 0) != pdTRUE) s_dropped++;
}

void telemetry_post_event(const event_t *event)
{
    item_t item = {.is_sample = false, .event = *event};
    post(&item);
}

void telemetry_post_sample(const telemetry_sample_t *sample)
{
    item_t item = {.is_sample = true, .sample = *sample};
    post(&item);
}

uint32_t telemetry_dropped(void)
{
    return s_dropped;
}
