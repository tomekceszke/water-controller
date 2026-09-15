#include <stdio.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"

#include "hi_ntp.h"

#include "config/config.h"
#include "events.h"
#include "valve.h"

static const char *TAG = "VALVE";
static const char *NVS_NAMESPACE = "wc_valve";
static const char *NVS_KEY = "state_v1";

typedef struct {
    uint8_t state;
    uint8_t reason;
    char detail[24];
    int64_t changed_unix_s;
} stored_t;

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t s_persist_lock;
static valve_info_t s_info;

const char *valve_reason_name(valve_reason_t reason)
{
    switch (reason) {
        case VALVE_BY_RESTORE: return "restore";
        case VALVE_BY_USER:    return "user";
        case VALVE_BY_ADMIN:   return "admin";
        case VALVE_BY_TIER1:   return "tier1";
        case VALVE_BY_TIER2:   return "tier2";
        default:               return "unknown";
    }
}

static int level_for(valve_state_t state)
{
    return state == VALVE_OPEN ? VALVE_LEVEL_OPEN : !VALVE_LEVEL_OPEN;
}

void valve_restore(void)
{
    s_persist_lock = xSemaphoreCreateMutex();
    stored_t stored = {.state = VALVE_OPEN, .reason = VALVE_BY_RESTORE};
    bool found = false;
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(stored);
        found = nvs_get_blob(h, NVS_KEY, &stored, &len) == ESP_OK && len == sizeof(stored)
                && stored.state <= VALVE_CLOSED;
        nvs_close(h);
    }
    if (!found) {
        // First boot of this firmware (legacy always drove the valve open at boot) or erased NVS
        stored = (stored_t) {.state = VALVE_OPEN, .reason = VALVE_BY_RESTORE};
    }

    // Level first, then output: the pin never drives the opposite state while being configured
    gpio_set_level(GPIO_VALVE, level_for(stored.state));
    gpio_config_t valve = {
        .pin_bit_mask = 1ULL << GPIO_VALVE,
        .mode = GPIO_MODE_INPUT_OUTPUT,     // readable for status
    };
    gpio_config(&valve);
    gpio_set_level(GPIO_VALVE, level_for(stored.state));

    gpio_config_t leds = {
        .pin_bit_mask = (1ULL << GPIO_LED_CLOSED) | (1ULL << GPIO_LED_FLOW),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&leds);
    gpio_set_level(GPIO_LED_CLOSED, stored.state == VALVE_CLOSED);
    gpio_set_level(GPIO_LED_FLOW, 0);

    s_info.state = (valve_state_t) stored.state;
    s_info.reason = (valve_reason_t) stored.reason;
    snprintf(s_info.detail, sizeof(s_info.detail), "%s", stored.detail);
    s_info.changed_unix_s = stored.changed_unix_s;
    s_info.changed_mono_ms = 0;

    if (found) {
        ESP_LOGW(TAG, "(not error) Restored valve %s (last changed by %s %s)",
                 stored.state == VALVE_CLOSED ? "CLOSED" : "open", valve_reason_name(stored.reason), stored.detail);
    } else {
        ESP_LOGE(TAG, "No stored valve state: valve kept OPEN (legacy behaviour)");
    }
}

static void persist(const valve_info_t *info)
{
    stored_t stored = {.state = (uint8_t) info->state, .reason = (uint8_t) info->reason,
                       .changed_unix_s = info->changed_unix_s};
    memcpy(stored.detail, info->detail, sizeof(stored.detail));
    xSemaphoreTake(s_persist_lock, portMAX_DELAY);
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, NVS_KEY, &stored, sizeof(stored));
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    }
    xSemaphoreGive(s_persist_lock);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Persisting valve state failed: %s", esp_err_to_name(err));
    }
}

void valve_set(valve_state_t state, valve_reason_t reason, const char *detail)
{
    int64_t now_ms = esp_timer_get_time() / 1000;
    valve_info_t next = {
        .state = state,
        .reason = reason,
        .changed_mono_ms = now_ms,
        .changed_unix_s = hi_ntp_synced() ? (int64_t) time(NULL) : 0,
    };
    snprintf(next.detail, sizeof(next.detail), "%s", detail ? detail : "");
    valve_info_t info;

    portENTER_CRITICAL(&s_mux);
    bool changed = s_info.state != state;
    if (changed) {
        gpio_set_level(GPIO_VALVE, level_for(state));       // the physical action comes first
        gpio_set_level(GPIO_LED_CLOSED, state == VALVE_CLOSED);
        s_info = next;
    }
    info = s_info;
    portEXIT_CRITICAL(&s_mux);

    if (!changed) return;
    ESP_LOGW(TAG, "(not error) Valve %s by %s %s", state == VALVE_CLOSED ? "CLOSED" : "OPENED",
             valve_reason_name(reason), info.detail);
    persist(&info);

    event_t e = {
        .type = EV_VALVE,
        .mono_ms = now_ms,
        .valve_state = (uint8_t) state,
        .reason = (uint8_t) reason,
    };
    snprintf(e.detail, sizeof(e.detail), "%s", info.detail);
    events_publish(&e);
}

valve_state_t valve_get(void)
{
    return s_info.state;
}

void valve_info(valve_info_t *out)
{
    portENTER_CRITICAL(&s_mux);
    *out = s_info;
    portEXIT_CRITICAL(&s_mux);
}
