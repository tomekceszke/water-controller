/*
 * Hardware test helpers, compiled only with -DWATER_TEST_PULSES=1 (spare board, never production):
 * POST /admin/test/pulses {"hz": n, "seconds": n} drives a square wave on the flow meter pin through LEDC,
 * so PCNT, Tier 1 and Tier 2 can be exercised without water. hz 0 stops.
 * POST /admin/test/wifi-off {"seconds": n} stops WiFi for n seconds (Tier 1 must keep working offline).
 * POST /admin/test/tier2 {"suspend": bool} suspends the Tier 2 task (Tier 1 must not care).
 */
#include "testhw.h"

#ifdef WATER_TEST_PULSES
#include "cJSON.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"

#include "hi_auth.h"
#include "hi_httpd.h"

#include "config/config.h"
#include "protect.h"

static const char *TAG = "TESTHW";
static esp_timer_handle_t s_stop_timer;
static esp_timer_handle_t s_wifi_timer;

static void wifi_on(void *arg)
{
    ESP_LOGW(TAG, "Test: WiFi back on");
    esp_wifi_start();
}

static bool admin_ok(httpd_req_t *req, esp_err_t *result)
{
    if (hi_auth_admin_header_valid(req)) return true;
    httpd_resp_set_status(req, "401 Unauthorized");
    *result = httpd_resp_send(req, "", 0);
    return false;
}

static esp_err_t wifi_off_handler(httpd_req_t *req)
{
    esp_err_t result;
    if (!admin_ok(req, &result)) return result;
    cJSON *body = hi_httpd_read_json(req, &result);
    if (body == NULL) return result;
    const cJSON *seconds = cJSON_GetObjectItemCaseSensitive(body, "seconds");
    uint32_t s = cJSON_IsNumber(seconds) ? (uint32_t) seconds->valuedouble : 0;
    cJSON_Delete(body);
    if (s == 0 || s > 600) return hi_httpd_send_error(req, "400 Bad Request", "seconds");
    result = hi_httpd_send_json(req, "200 OK", cJSON_CreateObject());
    ESP_LOGW(TAG, "Test: WiFi off for %lu s", (unsigned long) s);
    esp_timer_start_once(s_wifi_timer, (uint64_t) s * 1000000);
    esp_wifi_stop();
    return result;
}

static esp_err_t tier2_handler(httpd_req_t *req)
{
    esp_err_t result;
    if (!admin_ok(req, &result)) return result;
    cJSON *body = hi_httpd_read_json(req, &result);
    if (body == NULL) return result;
    bool suspend = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(body, "suspend"));
    cJSON_Delete(body);
    protect_test_suspend(suspend);
    ESP_LOGW(TAG, "Test: Tier 2 %s", suspend ? "suspended" : "resumed");
    return hi_httpd_send_json(req, "200 OK", cJSON_CreateObject());
}

static void stop_pulses(void *arg)
{
    ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
    ESP_LOGW(TAG, "Test pulses stopped");
}

static esp_err_t pulses_handler(httpd_req_t *req)
{
    if (!hi_auth_admin_header_valid(req)) {
        httpd_resp_set_status(req, "401 Unauthorized");
        return httpd_resp_send(req, "", 0);
    }
    esp_err_t result;
    cJSON *body = hi_httpd_read_json(req, &result);
    if (body == NULL) return result;
    const cJSON *hz = cJSON_GetObjectItemCaseSensitive(body, "hz");
    const cJSON *seconds = cJSON_GetObjectItemCaseSensitive(body, "seconds");
    uint32_t f = cJSON_IsNumber(hz) ? (uint32_t) hz->valuedouble : 0;
    uint32_t s = cJSON_IsNumber(seconds) ? (uint32_t) seconds->valuedouble : 0;
    cJSON_Delete(body);

    esp_timer_stop(s_stop_timer);
    if (f == 0 || s == 0) {
        stop_pulses(NULL);
    } else {
        ledc_timer_config(&(ledc_timer_config_t) {
            .speed_mode = LEDC_LOW_SPEED_MODE, .timer_num = LEDC_TIMER_0, .duty_resolution = LEDC_TIMER_8_BIT,
            .freq_hz = f, .clk_cfg = LEDC_AUTO_CLK,
        });
        ledc_channel_config(&(ledc_channel_config_t) {
            .gpio_num = GPIO_FLOW_METER, .speed_mode = LEDC_LOW_SPEED_MODE, .channel = LEDC_CHANNEL_0,
            .timer_sel = LEDC_TIMER_0, .duty = 128,
        });
        // LEDC took the pad as output: keep its input path enabled for PCNT
        gpio_input_enable(GPIO_FLOW_METER);
        esp_timer_start_once(s_stop_timer, (uint64_t) s * 1000000);
        ESP_LOGW(TAG, "Test pulses %lu Hz for %lu s", (unsigned long) f, (unsigned long) s);
    }
    cJSON *json = cJSON_CreateObject();
    cJSON_AddNumberToObject(json, "hz", f);
    cJSON_AddNumberToObject(json, "seconds", s);
    return hi_httpd_send_json(req, "200 OK", json);
}

void testhw_start(void)
{
    esp_timer_create(&(esp_timer_create_args_t) {.callback = stop_pulses, .name = "test_pulses"}, &s_stop_timer);
    esp_timer_create(&(esp_timer_create_args_t) {.callback = wifi_on, .name = "test_wifi"}, &s_wifi_timer);
    hi_httpd_register(&(httpd_uri_t) {.uri = "/admin/test/pulses", .method = HTTP_POST, .handler = pulses_handler});
    hi_httpd_register(&(httpd_uri_t) {.uri = "/admin/test/wifi-off", .method = HTTP_POST, .handler = wifi_off_handler});
    hi_httpd_register(&(httpd_uri_t) {.uri = "/admin/test/tier2", .method = HTTP_POST, .handler = tier2_handler});
    ESP_LOGE(TAG, "TEST BUILD: pulse generator enabled on the flow meter pin");
}
#else
void testhw_start(void)
{
}
#endif
