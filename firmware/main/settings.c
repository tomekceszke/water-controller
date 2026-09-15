#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "nvs.h"

#include "config/config.h"
#include "settings.h"

static const char *TAG = "SETTINGS";
static const char *NVS_NAMESPACE = "wc_settings";
static const char *NVS_KEY = "v1";

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t s_write_lock;
static settings_t s_settings;
static volatile uint32_t s_tier1_limit_s = TIER1_LIMIT_DEFAULT_S;

static uint32_t clamp(uint32_t v, uint32_t lo, uint32_t hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static void defaults(settings_t *s)
{
    memset(s, 0, sizeof(*s));
    s->tier1_limit_s = TIER1_LIMIT_DEFAULT_S;
    s->pulses_per_liter = PULSES_PER_LITER_DEFAULT;
    s->burst_lpm = 40;
    s->night_start_h = 0;
    s->night_end_h = 5;
    s->vacation_max_liters = 5;
}

static void sanitize(settings_t *s)
{
    s->tier1_limit_s = clamp(s->tier1_limit_s, TIER1_LIMIT_MIN_S, TIER1_LIMIT_MAX_S);
    s->pulses_per_liter = clamp(s->pulses_per_liter, PULSES_PER_LITER_MIN, PULSES_PER_LITER_MAX);
    s->max_event_liters = clamp(s->max_event_liters, 0, TIER2_MAX_LITERS_LIMIT);
    s->burst_lpm = clamp(s->burst_lpm, 1, TIER2_MAX_LPM_LIMIT);
    s->burst_s = clamp(s->burst_s, 0, TIER2_MAX_BURST_S);
    s->night_start_h %= 24;
    s->night_end_h %= 24;
    s->night_max_liters = clamp(s->night_max_liters, 0, TIER2_MAX_LITERS_LIMIT);
    s->leak_notify_min = clamp(s->leak_notify_min, 0, TIER2_MAX_LEAK_MIN);
    s->vacation = s->vacation ? true : false;
    s->vacation_max_liters = clamp(s->vacation_max_liters, 1, TIER2_MAX_LITERS_LIMIT);
}

static void apply(const settings_t *s)
{
    portENTER_CRITICAL(&s_mux);
    s_settings = *s;
    portEXIT_CRITICAL(&s_mux);
    s_tier1_limit_s = s->tier1_limit_s;
}

void settings_init(void)
{
    s_write_lock = xSemaphoreCreateMutex();
    settings_t s;
    defaults(&s);

    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        settings_t stored;
        size_t len = sizeof(stored);
        if (nvs_get_blob(h, NVS_KEY, &stored, &len) == ESP_OK && len == sizeof(stored)) {
            s = stored;
        } else {
            ESP_LOGW(TAG, "No stored settings, using defaults");
        }
        nvs_close(h);
    } else {
        ESP_LOGW(TAG, "No stored settings, using defaults");
    }
    sanitize(&s);
    apply(&s);
    ESP_LOGI(TAG, "Tier 1 limit %lu s, %lu pulses/L", (unsigned long) s.tier1_limit_s,
             (unsigned long) s.pulses_per_liter);
}

void settings_get(settings_t *out)
{
    portENTER_CRITICAL(&s_mux);
    *out = s_settings;
    portEXIT_CRITICAL(&s_mux);
}

uint32_t settings_tier1_limit_s(void)
{
    return s_tier1_limit_s;
}

esp_err_t settings_set(settings_t *in_out)
{
    sanitize(in_out);
    xSemaphoreTake(s_write_lock, portMAX_DELAY);
    apply(in_out);      // effective immediately, even if persisting fails
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, NVS_KEY, in_out, sizeof(*in_out));
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    }
    xSemaphoreGive(s_write_lock);
    if (err != ESP_OK) ESP_LOGE(TAG, "Saving settings failed: %s", esp_err_to_name(err));
    return err;
}
