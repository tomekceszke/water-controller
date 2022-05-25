#include <stdint.h>
#include <esp_err.h>
#include "freertos/FreeRTOS.h"
#include <freertos/task.h>
#include <driver/gpio.h>
#include "esp_log.h"

#include "udp_logging.h"
#include "config.h"

static const char *TAG = "MAIN";

esp_err_t connect();

void httpd(int port);

esp_err_t init_nvs();

void ota();

_Noreturn void pcnt();

void ntp();

void reset_gpio();


void app_main(void) {
    ESP_LOGI(TAG, "Connecting to AP...");
    connect();
    ESP_LOGW(TAG, "Checking OTA...");
    ota();
    ESP_LOGI(TAG, "Init UDP logging...");
    udp_logging_init(CONFIG_LOG_UDP_IP, CONFIG_LOG_UDP_PORT, udp_logging_vprintf);
    ESP_LOGI(TAG, "Init GPIO...");
    reset_gpio();
    //ESP_LOGI(TAG, "Free heap size: %d", xPortGetFreeHeapSize());
    ESP_LOGI(TAG, "Starting web server...");
    httpd(PORT_WEB);
    //ESP_LOGI(TAG, "Free heap size: %d", xPortGetFreeHeapSize());
    ESP_LOGI(TAG, "Setting time...");
    ntp();
    //ESP_LOGI(TAG, "Free heap size: %d", xPortGetFreeHeapSize());
    ESP_LOGI(TAG, "Starting counter...");
    xTaskCreate(&pcnt, "pcnt", 16384, NULL, 5, NULL);
    ESP_LOGI(TAG, "All done! Free heap size: %d", xPortGetFreeHeapSize());
    ESP_LOGE(TAG, "(not error) Built: %s %s", __DATE__, __TIME__);
}
