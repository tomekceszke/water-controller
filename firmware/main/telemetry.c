#include <inttypes.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "mqtt_client.h"

#include "hi_ntp.h"

#include "config/config.h"
#include "rules.h"
#include "settings.h"
#include "telemetry.h"
#include "valve.h"

/*
 * Publishes to Mosquitto on hc-data (QoS 1, esp-mqtt outbox bounded by MQTT_OUTBOX_LIMIT_BYTES):
 *   water/<mac>/flow     {"start":<unix>,"stop":<unix>,"pulses":n,"liters":x,"max_lpm":x,"closed":bool}
 *   water/<mac>/sample   {"ts":<unix>,"pulses":n,"period_ms":n,"lpm":x,"flow_pulses":n}
 *   water/<mac>/valve    {"ts":<unix>,"state":"open|closed","reason":"...","detail":"..."}
 *   water/<mac>/rule     {"ts":<unix>,"rule":"...","close":bool,"pulses":n,"detail":"..."}
 *   water/<mac>/alert    {"ts":<unix>,"detail":"..."}
 *   water/<mac>/status   "online" / "offline" (retained, LWT)
 * Items created before the clock is synced wait in the queue and get their wall-clock time once it is.
 */

static const char *TAG = "TELEMETRY";

#define TOPIC_MAX_LEN 64
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
static esp_mqtt_client_handle_t s_client;
static char s_device_id[13];
static char s_client_id[16];
static char s_status_topic[TOPIC_MAX_LEN];
static volatile bool s_connected;
static volatile uint32_t s_dropped;

static int64_t unix_of(int64_t mono_ms)
{
    return events_unix_time(mono_ms);
}

static void publish(const char *kind, const char *payload)
{
    char topic[TOPIC_MAX_LEN];
    snprintf(topic, sizeof(topic), MQTT_TOPIC_PREFIX "/%s/%s", s_device_id, kind);
    if (esp_mqtt_client_enqueue(s_client, topic, payload, 0, 1, 0, true) < 0) {
        s_dropped++;
        ESP_LOGW(TAG, "Outbox full, dropped %s", kind);
    }
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

static void on_mqtt_event(void *arg, esp_event_base_t base, int32_t event_id, void *event_data)
{
    const esp_mqtt_event_handle_t event = event_data;
    switch ((esp_mqtt_event_id_t) event_id) {
        case MQTT_EVENT_CONNECTED:
            s_connected = true;
            ESP_LOGI(TAG, "MQTT connected, %d bytes queued", esp_mqtt_client_get_outbox_size(s_client));
            esp_mqtt_client_enqueue(s_client, s_status_topic, "online", 0, 1, 1, true);
            break;
        case MQTT_EVENT_DISCONNECTED:
            s_connected = false;
            ESP_LOGW(TAG, "MQTT disconnected");
            break;
        case MQTT_EVENT_ERROR:
            // Warning, not error: an unreachable hc-data must not turn into error notifications
            ESP_LOGW(TAG, "MQTT error type %d, errno %d", event->error_handle->error_type,
                     event->error_handle->esp_transport_sock_errno);
            break;
        default:
            break;
    }
}

void telemetry_start(const char *mqtt_pass)
{
    if (mqtt_pass == NULL || mqtt_pass[0] == '\0') {
        ESP_LOGW(TAG, "No MQTT password: telemetry disabled");
        return;
    }
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_device_id, sizeof(s_device_id), "%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    snprintf(s_client_id, sizeof(s_client_id), "wc-%s", s_device_id);
    snprintf(s_status_topic, sizeof(s_status_topic), MQTT_TOPIC_PREFIX "/%s/status", s_device_id);

    const esp_mqtt_client_config_t config = {
        .broker.address.uri = MQTT_BROKER_URI,
        .credentials = {
            .username = TELEMETRY_MQTT_USER,
            .client_id = s_client_id,
            .authentication.password = mqtt_pass,
        },
        .session.last_will = {.topic = s_status_topic, .msg = "offline", .qos = 1, .retain = 1},
        .outbox.limit = MQTT_OUTBOX_LIMIT_BYTES,
        .network.reconnect_timeout_ms = 30000,
    };
    s_client = esp_mqtt_client_init(&config);
    s_queue = xQueueCreate(TELEMETRY_QUEUE_LEN, sizeof(item_t));
    if (s_client == NULL || s_queue == NULL) {
        ESP_LOGW(TAG, "Telemetry disabled: out of memory");
        s_queue = NULL;
        return;
    }
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, on_mqtt_event, NULL);
    esp_mqtt_client_start(s_client);
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

void telemetry_stats(telemetry_stats_t *out)
{
    out->enabled = s_queue != NULL;
    out->connected = s_connected;
    out->dropped = s_dropped;
    out->outbox_bytes = s_client ? esp_mqtt_client_get_outbox_size(s_client) : 0;
}
