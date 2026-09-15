#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "hi_auth.h"
#include "hi_httpd.h"

#include "api.h"
#include "config/config.h"
#include "events.h"
#include "flow.h"
#include "protect.h"
#include "rules.h"
#include "settings.h"
#include "telemetry.h"
#include "valve.h"

static const char *TAG = "API";

extern const uint8_t login_html_gz_start[] asm("_binary_login_html_gz_start");
extern const uint8_t login_html_gz_end[] asm("_binary_login_html_gz_end");
extern const uint8_t app_html_gz_start[] asm("_binary_app_html_gz_start");
extern const uint8_t app_html_gz_end[] asm("_binary_app_html_gz_end");
extern const uint8_t icon_png_start[] asm("_binary_apple_touch_icon_png_start");
extern const uint8_t icon_png_end[] asm("_binary_apple_touch_icon_png_end");
extern const char manifest_start[] asm("_binary_manifest_webmanifest_start");

static double liters(uint64_t pulses, uint32_t ppl)
{
    return ppl ? (double) pulses / ppl : 0;
}

static void add_settings(cJSON *obj, const settings_t *s)
{
    cJSON_AddNumberToObject(obj, "tier1_limit_s", s->tier1_limit_s);
    cJSON_AddNumberToObject(obj, "tier1_limit_min_s", TIER1_LIMIT_MIN_S);
    cJSON_AddNumberToObject(obj, "tier1_limit_max_s", TIER1_LIMIT_MAX_S);
    cJSON_AddNumberToObject(obj, "pulses_per_liter", s->pulses_per_liter);
    cJSON_AddNumberToObject(obj, "max_event_liters", s->max_event_liters);
    cJSON_AddNumberToObject(obj, "burst_lpm", s->burst_lpm);
    cJSON_AddNumberToObject(obj, "burst_s", s->burst_s);
    cJSON_AddNumberToObject(obj, "night_start_h", s->night_start_h);
    cJSON_AddNumberToObject(obj, "night_end_h", s->night_end_h);
    cJSON_AddNumberToObject(obj, "night_max_liters", s->night_max_liters);
    cJSON_AddNumberToObject(obj, "leak_notify_min", s->leak_notify_min);
    cJSON_AddBoolToObject(obj, "vacation", s->vacation);
    cJSON_AddNumberToObject(obj, "vacation_max_liters", s->vacation_max_liters);
}

static cJSON *status_json(void)
{
    int64_t now_ms = esp_timer_get_time() / 1000;
    settings_t s;
    settings_get(&s);
    cJSON *root = cJSON_CreateObject();

    valve_info_t v;
    valve_info(&v);
    cJSON *valve = cJSON_AddObjectToObject(root, "valve");
    cJSON_AddStringToObject(valve, "state", v.state == VALVE_CLOSED ? "closed" : "open");
    cJSON_AddStringToObject(valve, "reason", valve_reason_name(v.reason));
    cJSON_AddStringToObject(valve, "detail", v.detail);
    cJSON_AddNumberToObject(valve, "changed_at", (double) (v.changed_mono_ms ? events_unix_time(v.changed_mono_ms)
                                                                            : v.changed_unix_s));

    flow_status_t f;
    flow_status(&f);
    cJSON *flow = cJSON_AddObjectToObject(root, "flow");
    cJSON_AddBoolToObject(flow, "flowing", f.flowing);
    cJSON_AddNumberToObject(flow, "lpm", f.lpm_x10 / 10.0);
    cJSON_AddNumberToObject(flow, "liters", liters(f.flow_pulses, s.pulses_per_liter));
    cJSON_AddNumberToObject(flow, "seconds", f.flow_ms / 1000);
    cJSON_AddNumberToObject(flow, "max_lpm", f.flow_max_lpm_x10 / 10.0);
    cJSON_AddNumberToObject(flow, "tier1_limit_s", f.tier1_limit_s);
    cJSON_AddNumberToObject(flow, "tier1_remaining_s", f.tier1_remaining_s);
    cJSON_AddBoolToObject(flow, "tier1_alive", now_ms - f.heartbeat_ms < 5 * FLOW_SAMPLE_MS);
    cJSON_AddNumberToObject(flow, "counter", (double) f.counter);

    events_totals_t t;
    events_totals(&t);
    cJSON *usage = cJSON_AddObjectToObject(root, "usage");
    cJSON_AddNumberToObject(usage, "liters_today", liters(t.pulses_today, s.pulses_per_liter));
    cJSON_AddNumberToObject(usage, "liters_since_boot", liters(t.pulses_since_boot, s.pulses_per_liter));
    cJSON_AddNumberToObject(usage, "flows_since_boot", t.flows_since_boot);

    protect_status_t p;
    protect_status(&p);
    cJSON *tier2 = cJSON_AddObjectToObject(root, "tier2");
    cJSON_AddNumberToObject(tier2, "snooze_left_s", p.snooze_left_s);
    cJSON_AddStringToObject(tier2, "last_rule", p.last_rule);
    cJSON_AddNumberToObject(tier2, "last_rule_at", (double) (p.last_rule_mono_ms ? events_unix_time(p.last_rule_mono_ms) : 0));
    cJSON_AddBoolToObject(tier2, "last_rule_closed", p.last_rule_closed);
    cJSON_AddNumberToObject(tier2, "dropped_samples", p.dropped_samples);

    add_settings(cJSON_AddObjectToObject(root, "settings"), &s);

    telemetry_stats_t ts;
    telemetry_stats(&ts);
    cJSON *tele = cJSON_AddObjectToObject(root, "telemetry");
    cJSON_AddBoolToObject(tele, "enabled", ts.enabled);
    cJSON_AddBoolToObject(tele, "connected", ts.connected);
    cJSON_AddNumberToObject(tele, "dropped", ts.dropped);
    cJSON_AddNumberToObject(tele, "outbox_bytes", ts.outbox_bytes);

    cJSON *diag = cJSON_AddObjectToObject(root, "diag");
    cJSON_AddBoolToObject(diag, "running", f.diag_running);
    cJSON_AddNumberToObject(diag, "left_s", f.diag_left_s);
    cJSON_AddNumberToObject(diag, "edges", f.diag_edges);
    cJSON_AddNumberToObject(diag, "short_intervals", f.diag_short);
    cJSON_AddNumberToObject(diag, "min_interval_us", f.diag_min_interval_us == UINT32_MAX ? 0 : f.diag_min_interval_us);

    hi_httpd_add_system_status(cJSON_AddObjectToObject(root, "system"));
    return root;
}

static esp_err_t status_handler(httpd_req_t *req)
{
    esp_err_t result;
    if (!hi_httpd_guard(req, HI_GUARD_SESSION, NULL, &result)) return result;
    return hi_httpd_send_json(req, "200 OK", status_json());
}

static esp_err_t events_handler(httpd_req_t *req)
{
    esp_err_t result;
    if (!hi_httpd_guard(req, HI_GUARD_SESSION, NULL, &result)) return result;
    static event_t list[EVENTS_RING_SIZE];      // httpd runs one handler at a time
    size_t n = events_recent(list, EVENTS_RING_SIZE);
    settings_t s;
    settings_get(&s);

    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(root, "events");
    for (size_t i = 0; i < n; i++) {
        const event_t *e = &list[i];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "ts", (double) events_unix_time(e->mono_ms));
        switch (e->type) {
            case EV_FLOW_END:
                cJSON_AddStringToObject(o, "type", "flow");
                cJSON_AddNumberToObject(o, "seconds", e->duration_ms / 1000);
                cJSON_AddNumberToObject(o, "liters", liters(e->pulses, s.pulses_per_liter));
                cJSON_AddNumberToObject(o, "max_lpm", e->max_lpm_x10 / 10.0);
                cJSON_AddBoolToObject(o, "closed", e->closed);
                break;
            case EV_VALVE:
                cJSON_AddStringToObject(o, "type", "valve");
                cJSON_AddStringToObject(o, "state", e->valve_state == VALVE_CLOSED ? "closed" : "open");
                cJSON_AddStringToObject(o, "reason", valve_reason_name((valve_reason_t) e->reason));
                cJSON_AddStringToObject(o, "detail", e->detail);
                break;
            case EV_RULE:
                cJSON_AddStringToObject(o, "type", "rule");
                cJSON_AddStringToObject(o, "rule", rules_name((rule_t) e->reason));
                cJSON_AddBoolToObject(o, "closed", e->closed);
                cJSON_AddStringToObject(o, "detail", e->detail);
                break;
            case EV_ALERT:
                cJSON_AddStringToObject(o, "type", "alert");
                cJSON_AddStringToObject(o, "detail", e->detail);
                break;
        }
        cJSON_AddItemToArray(arr, o);
    }
    return hi_httpd_send_json(req, "200 OK", root);
}

static bool parse_valve_state(const cJSON *body, valve_state_t *out)
{
    const cJSON *state = cJSON_GetObjectItemCaseSensitive(body, "state");
    if (!cJSON_IsString(state)) return false;
    if (strcmp(state->valuestring, "open") == 0) *out = VALVE_OPEN;
    else if (strcmp(state->valuestring, "closed") == 0) *out = VALVE_CLOSED;
    else return false;
    return true;
}

static esp_err_t valve_handler(httpd_req_t *req)
{
    esp_err_t result;
    if (!hi_httpd_guard(req, HI_GUARD_MUTATION, NULL, &result)) return result;
    cJSON *body = hi_httpd_read_json(req, &result);
    if (body == NULL) return result;
    valve_state_t state;
    bool ok = parse_valve_state(body, &state);
    cJSON_Delete(body);
    if (!ok) return hi_httpd_send_error(req, "400 Bad Request", "state");

    char ip[16];
    hi_httpd_client_ip(req, ip, sizeof(ip));
    valve_set(state, VALVE_BY_USER, ip);
    return hi_httpd_send_json(req, "200 OK", status_json());
}

static void json_u32(const cJSON *obj, const char *key, uint32_t *field)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsNumber(item)) *field = item->valuedouble < 0 ? 0 : (uint32_t) item->valuedouble;
}

static void json_u8(const cJSON *obj, const char *key, uint8_t *field)
{
    uint32_t v = *field;
    json_u32(obj, key, &v);
    *field = (uint8_t) (v > 255 ? 255 : v);
}

static esp_err_t settings_handler(httpd_req_t *req)
{
    esp_err_t result;
    if (!hi_httpd_guard(req, HI_GUARD_MUTATION, NULL, &result)) return result;
    cJSON *body = hi_httpd_read_json(req, &result);
    if (body == NULL) return result;

    settings_t s;
    settings_get(&s);
    json_u32(body, "tier1_limit_s", &s.tier1_limit_s);
    json_u32(body, "pulses_per_liter", &s.pulses_per_liter);
    json_u32(body, "max_event_liters", &s.max_event_liters);
    json_u32(body, "burst_lpm", &s.burst_lpm);
    json_u32(body, "burst_s", &s.burst_s);
    json_u8(body, "night_start_h", &s.night_start_h);
    json_u8(body, "night_end_h", &s.night_end_h);
    json_u32(body, "night_max_liters", &s.night_max_liters);
    json_u32(body, "leak_notify_min", &s.leak_notify_min);
    const cJSON *vacation = cJSON_GetObjectItemCaseSensitive(body, "vacation");
    if (cJSON_IsBool(vacation)) s.vacation = cJSON_IsTrue(vacation);
    json_u32(body, "vacation_max_liters", &s.vacation_max_liters);
    cJSON_Delete(body);

    esp_err_t err = settings_set(&s);
    ESP_LOGW(TAG, "(not error) Settings changed: Tier 1 %lu s", (unsigned long) s.tier1_limit_s);
    if (err != ESP_OK) return hi_httpd_send_error(req, "500 Internal Server Error", "save_failed");
    return hi_httpd_send_json(req, "200 OK", status_json());
}

static esp_err_t minutes_from_body(httpd_req_t *req, uint32_t *minutes, esp_err_t *result)
{
    cJSON *body = hi_httpd_read_json(req, result);
    if (body == NULL) return ESP_FAIL;
    const cJSON *m = cJSON_GetObjectItemCaseSensitive(body, "minutes");
    bool ok = cJSON_IsNumber(m) && m->valuedouble >= 0;
    *minutes = ok ? (uint32_t) m->valuedouble : 0;
    cJSON_Delete(body);
    if (!ok) *result = hi_httpd_send_error(req, "400 Bad Request", "minutes");
    return ok ? ESP_OK : ESP_FAIL;
}

static esp_err_t snooze_handler(httpd_req_t *req)
{
    esp_err_t result;
    if (!hi_httpd_guard(req, HI_GUARD_MUTATION, NULL, &result)) return result;
    uint32_t minutes;
    if (minutes_from_body(req, &minutes, &result) != ESP_OK) return result;
    protect_snooze(minutes);
    return hi_httpd_send_json(req, "200 OK", status_json());
}

static esp_err_t diag_handler(httpd_req_t *req)
{
    esp_err_t result;
    if (!hi_httpd_guard(req, HI_GUARD_MUTATION, NULL, &result)) return result;
    uint32_t minutes;
    if (minutes_from_body(req, &minutes, &result) != ESP_OK) return result;
    flow_diag(minutes);
    return hi_httpd_send_json(req, "200 OK", status_json());
}

/* Scripts / home automation: Authorization header, same body as /api/valve. */
static esp_err_t admin_valve_handler(httpd_req_t *req)
{
    if (!hi_auth_admin_header_valid(req)) {
        httpd_resp_set_status(req, "401 Unauthorized");
        return httpd_resp_send(req, "", 0);
    }
    esp_err_t result;
    cJSON *body = hi_httpd_read_json(req, &result);
    if (body == NULL) return result;
    valve_state_t state;
    bool ok = parse_valve_state(body, &state);
    cJSON_Delete(body);
    if (!ok) return hi_httpd_send_error(req, "400 Bad Request", "state");
    valve_set(state, VALVE_BY_ADMIN, "admin");
    return hi_httpd_send_json(req, "200 OK", status_json());
}

static esp_err_t admin_status_handler(httpd_req_t *req)
{
    if (!hi_auth_admin_header_valid(req)) {
        httpd_resp_set_status(req, "401 Unauthorized");
        return httpd_resp_send(req, "", 0);
    }
    return hi_httpd_send_json(req, "200 OK", status_json());
}

void api_start(void)
{
    static const char *const hosts[] = HTTPD_ALLOWED_HOSTS;
    static hi_httpd_ui_t ui;
    ui = (hi_httpd_ui_t) {
        .login_html_gz = {login_html_gz_start, login_html_gz_end},
        .app_html_gz = {app_html_gz_start, app_html_gz_end},
        .icon_png = {icon_png_start, icon_png_end},
        .manifest_json = manifest_start,
    };
    if (hi_httpd_start(&(hi_httpd_config_t) {
            .allowed_hosts = hosts,
            .allowed_hosts_count = sizeof(hosts) / sizeof(hosts[0]),
            .ui = &ui,
            .max_uri_handlers = 24,
        }) != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server not started (protection unaffected)");
        return;
    }
    const httpd_uri_t routes[] = {
        {.uri = "/api/status", .method = HTTP_GET, .handler = status_handler},
        {.uri = "/api/events", .method = HTTP_GET, .handler = events_handler},
        {.uri = "/api/valve", .method = HTTP_POST, .handler = valve_handler},
        {.uri = "/api/settings", .method = HTTP_POST, .handler = settings_handler},
        {.uri = "/api/snooze", .method = HTTP_POST, .handler = snooze_handler},
        {.uri = "/api/diag", .method = HTTP_POST, .handler = diag_handler},
        {.uri = "/admin/valve", .method = HTTP_POST, .handler = admin_valve_handler},
        {.uri = "/admin/status", .method = HTTP_GET, .handler = admin_status_handler},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        hi_httpd_register(&routes[i]);
    }
}
