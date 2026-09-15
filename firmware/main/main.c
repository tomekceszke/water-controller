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
#include "hi_secret.h"
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
#include "testhw.h"
#include "valve.h"

static const char *TAG = "MAIN";

extern const char ota_cert_pem_start[] asm("_binary_ota_server_cert_15_pem_start");

/* credentials.h values may be obfuscated ("obf1:...", home-idf tools/obfuscate.py) */
static char s_wifi_pass[65];
static char s_admin_header[128];
static char s_ntfy_topic[65];
static char s_ntfy_error_topic[65];
static char s_mqtt_pass[65];

static void reveal_credentials(void)
{
    bool ok = hi_secret_reveal(WIFI_PASS, s_wifi_pass, sizeof(s_wifi_pass))
              & hi_secret_reveal(HEADER_AUTHORIZATION_VALUE, s_admin_header, sizeof(s_admin_header))
              & hi_secret_reveal(NTFY_TOPIC, s_ntfy_topic, sizeof(s_ntfy_topic))
              & hi_secret_reveal(NTFY_ERROR_TOPIC, s_ntfy_error_topic, sizeof(s_ntfy_error_topic))
              & hi_secret_reveal(MQTT_PASS, s_mqtt_pass, sizeof(s_mqtt_pass));
    if (!ok) ESP_LOGE(TAG, "A credential in credentials.h is malformed");
}

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
    reveal_credentials();
    hi_wifi_start(&(hi_wifi_config_t) {.ssid = WIFI_SSID, .password = s_wifi_pass, .hostname = DEVICE_HOSTNAME});
    hi_notify_init(&(hi_notify_config_t) {
        .topic = s_ntfy_topic,
        .error_topic = s_ntfy_error_topic,
        .error_title = "Water controller error",
        .click_url = APP_URL,
        .error_cooldown_s = NOTIFY_ERROR_COOLDOWN_S,
    });
    hi_ntp_start(&(hi_ntp_config_t) {.servers = {"0.pl.pool.ntp.org", "1.pl.pool.ntp.org", "pool.ntp.org"}});
    hi_ota_init(&(hi_ota_config_t) {.url = OTA_URL, .cert_pem = ota_cert_pem_start, .delete_after = true});
    telemetry_start(s_mqtt_pass);
    hi_auth_init(&(hi_auth_config_t) {
        .password_iterations = AUTH_PASSWORD_ITERATIONS,
#ifdef WATER_TEST_SALT_HEX      // hardware test builds: throwaway password, never the owner's
        .password_salt_hex = WATER_TEST_SALT_HEX,
        .password_hash_hex = WATER_TEST_HASH_HEX,
#else
        .password_salt_hex = AUTH_PASSWORD_SALT_HEX,
        .password_hash_hex = AUTH_PASSWORD_HASH_HEX,
#endif
        .admin_header_value = s_admin_header,
    });
    api_start();
    testhw_start();
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
