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

void start_web_server(int port);

esp_err_t init_nvs();

void ota_task();

_Noreturn void pcnt_task();

void ntp_task();


void initGPIO() {
    gpio_pad_select_gpio(FLOW_METER_IN_GPIO);
    gpio_set_direction(FLOW_METER_IN_GPIO, GPIO_MODE_INPUT);
    gpio_pulldown_en(FLOW_METER_IN_GPIO);

    gpio_pad_select_gpio(VALVE_CTRL_OUT_GPIO);
    gpio_set_direction(VALVE_CTRL_OUT_GPIO, GPIO_MODE_INPUT_OUTPUT);
    // gpio_pulldown_en(VALVE_CTRL_OUT_GPIO);
    gpio_set_level(VALVE_CTRL_OUT_GPIO, 1);

    gpio_pad_select_gpio(LED_RED_OUT_GPIO);
    gpio_set_direction(LED_RED_OUT_GPIO, GPIO_MODE_INPUT_OUTPUT);
    gpio_set_level(LED_RED_OUT_GPIO, 0);

    gpio_pad_select_gpio(LED_BLUE_OUT_GPIO);
    gpio_set_direction(LED_BLUE_OUT_GPIO, GPIO_MODE_INPUT_OUTPUT);
    gpio_set_level(LED_BLUE_OUT_GPIO, 0);
}

void app_main(void) {
    ESP_LOGI(TAG, "Connecting to AP...");
    if (ESP_OK == connect()) {
        ESP_LOGI(TAG, "Connected.");
    } else {
        ESP_LOGE(TAG, "Can't connect!");
        return;
    }
    ESP_LOGW(TAG, "Checking OTA...");
    ota_task();
    ESP_LOGI(TAG, "Init UDP logging...");
    udp_logging_init(CONFIG_LOG_UDP_IP, CONFIG_LOG_UDP_PORT, udp_logging_vprintf);
    ESP_LOGI(TAG, "Init GPIO...");
    initGPIO();
    ESP_LOGI(TAG, "Starting web server...");
    start_web_server(PORT_WEB);

    ESP_LOGI(TAG, "Getting time...");
    xTaskCreate(&ntp_task, "ntp_task", 2048, NULL, 5, NULL);

    ESP_LOGI(TAG, "Starting counter...");
    xTaskCreate(&pcnt_task, "pcnt_task", 16384, NULL, 5, NULL);

    ESP_LOGI(TAG, "All done!");
    ESP_LOGE(TAG, "(not error) Built: %s %s", __DATE__, __TIME__);
}
