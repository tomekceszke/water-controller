#include <stdint.h>
#include <esp_err.h>
#include "freertos/FreeRTOS.h"
#include <freertos/task.h>
#include "nvs_flash.h"
#include "esp_log.h"

#include "udp_logging.h"
#include "config.h"

static const char *TAG = "MAIN";

void connect();

void httpd(int port);

void ota();

_Noreturn void pcnt();

void ntp();

void reset_gpio();


void init_nvs() {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
}

void app_main(void) {
    ESP_LOGI(TAG, "\n\n");
    ESP_LOGI(TAG, "Initializing NVS...");
    init_nvs();
    ESP_LOGI(TAG, "Connecting to AP...");
    connect();
    ESP_LOGI(TAG, "Init UDP logging...");
    udp_logging_init(CONFIG_LOG_UDP_IP, CONFIG_LOG_UDP_PORT, udp_logging_vprintf);
    ESP_LOGW(TAG, "Checking OTA...");
    ota();
    ESP_LOGI(TAG, "Init GPIO...");
    reset_gpio();
    ESP_LOGI(TAG, "Starting web server...");
    httpd(PORT_WEB);
    ESP_LOGI(TAG, "Setting time...");
    ntp();
    ESP_LOGI(TAG, "Starting counter...");
    xTaskCreate(&pcnt, "pcnt", 16384, NULL, 5, NULL);
    ESP_LOGI(TAG, "All done! Free heap size: %d", xPortGetFreeHeapSize());
    ESP_LOGE(TAG, "(not error) Built: %s %s", __DATE__, __TIME__);
}
