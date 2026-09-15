#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_app_desc.h"
#include "esp_log.h"

#include "hi_auth.h"
#include "hi_health.h"
#include "hi_log.h"
#include "hi_notify.h"
#include "hi_ntp.h"
#include "hi_ota.h"
#include "hi_system.h"
#include "hi_wifi.h"

#include "api.h"
#include "config/config.h"
#include "config/credentials.h"
#include "events.h"
#include "flow.h"
#include "protect.h"
#include "settings.h"
#include "telemetry.h"
#include "valve.h"

static const char *TAG = "MAIN";

extern const char ota_cert_pem_start[] asm("_binary_ota_server_cert_15_pem_start");

static bool healthy(void)
{
    return flow_alive() && hi_wifi_is_connected();
}

static void log_stats(void)
{
    flow_status_t f;
    flow_status(&f);
    ESP_LOGI(TAG, "heap %u KB (min %u) | rssi %d | valve %s | counter %llu",
             (unsigned) (esp_get_free_heap_size() / 1024), (unsigned) (esp_get_minimum_free_heap_size() / 1024),
             hi_wifi_rssi(), valve_get() == VALVE_CLOSED ? "closed" : "open", (unsigned long long) f.counter);
}

void app_main(void)
{
    /* ---- Tier 1 first: nothing below this block may delay or prevent it ---- */
    bool nvs_erased = false;
    hi_nvs_init(&nvs_erased);
    valve_restore();            // same valve state as before the reset
    settings_init();
    events_init();
    flow_start();               // Tier 1 task

    hi_health_early_boot(HEALTH_MAX_UNVERIFIED_BOOTS);
#ifdef WATER_TEST_PANIC
    ESP_LOGE(TAG, "WATER_TEST_PANIC build: crashing on purpose to test the rollback");
    abort();
#endif

    /* ---- everything else is optional ---- */
    hi_log_init(&(hi_log_config_t) {.udp_ip = LOG_UDP_IP, .udp_port = LOG_UDP_PORT, .on_error_line = hi_notify_error});
    ESP_LOGW(TAG, "(not error) water-controller %s, reset: %s", esp_app_get_description()->version, hi_reset_reason());
    if (nvs_erased) ESP_LOGE(TAG, "NVS was erased: valve state and settings reset to defaults");

    protect_start();            // Tier 2
    hi_wifi_start(&(hi_wifi_config_t) {.ssid = WIFI_SSID, .password = WIFI_PASS, .hostname = DEVICE_HOSTNAME});
    hi_notify_init(&(hi_notify_config_t) {
        .topic = NTFY_TOPIC,
        .error_topic = NTFY_ERROR_TOPIC,
        .error_title = "Water controller error",
        .click_url = APP_URL,
        .error_cooldown_s = NOTIFY_ERROR_COOLDOWN_S,
    });
    hi_ntp_start(&(hi_ntp_config_t) {.servers = {"0.pl.pool.ntp.org", "1.pl.pool.ntp.org", "pool.ntp.org"}});
    hi_ota_init(&(hi_ota_config_t) {.url = OTA_URL, .cert_pem = ota_cert_pem_start, .delete_after = true});
    telemetry_start();
    hi_auth_init(&(hi_auth_config_t) {
        .password_iterations = AUTH_PASSWORD_ITERATIONS,
        .password_salt_hex = AUTH_PASSWORD_SALT_HEX,
        .password_hash_hex = AUTH_PASSWORD_HASH_HEX,
        .admin_header_value = HEADER_AUTHORIZATION_VALUE,
    });
    api_start();
    hi_health_start(&(hi_health_config_t) {
        .is_healthy = healthy,
        .log_stats = log_stats,
        .verify_timeout_s = HEALTH_VERIFY_TIMEOUT_S,
        .stats_period_s = HEALTH_STATS_LOG_PERIOD_S,
    });

    if (hi_wifi_wait_connected(60000)) {
        hi_ota_start_background();
    } else {
        ESP_LOGW(TAG, "No WiFi after 60 s: OTA check skipped (protection running)");
    }
    hi_notify_event_ex("Water controller", "Started", HI_NOTIFY_PRIO_MIN, NULL);
}
