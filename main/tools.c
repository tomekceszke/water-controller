#include <esp_sntp.h>
#include <esp_log.h>
#include "config.h"

static const char *TAG = "TOOLS";

char boot_time[64];

void led(gpio_num_t pin, bool on) {
    gpio_set_level(pin, on);
}

bool is_valve_closed() {
    return !gpio_get_level(VALVE_CTRL_OUT_GPIO);
}

void close_valve(bool close) {
    ESP_LOGW(TAG, "Valve is %s", (close ? "closing" : "opening"));
    led(LED_RED_OUT_GPIO, close);
    gpio_set_level(VALVE_CTRL_OUT_GPIO, !close);
}


void ntp() {
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, "0.pl.pool.ntp.org");
    sntp_setservername(1, "1.pl.pool.ntp.org");
    sntp_setservername(2, "2.pl.pool.ntp.org");

    sntp_init();
    time_t now = 0;
    time(&now);

    u_short i = 0;
    while (now < 5000 || i > NTP_MAX_ATTEMPTS) {
        ESP_LOGI(TAG, "Getting time, attempt: %d", ++i);
        vTaskDelay(1000 * portTICK_RATE_MS);
        time(&now);
    }

    if(now < 5000) {
        ESP_LOGE(TAG, "Couldn't get current time by NTP. Some features won't work!");
        return;
    }

    struct tm timeinfo = { 0 };

    setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
    tzset();

    localtime_r(&now, &timeinfo);
    strftime(boot_time, sizeof(boot_time), "%c", &timeinfo);
    ESP_LOGI(TAG, "The current local date/time is: %s", boot_time);
}

void ntp_task() {
    ntp();
    vTaskDelete(NULL);
}



