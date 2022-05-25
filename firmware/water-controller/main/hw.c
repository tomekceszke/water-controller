#include <esp_log.h>
#include "config.h"
#include "common.h"

static const char *TAG = "HW";

void send_state(enum State state);

void reset_gpio() {
    esp_rom_gpio_pad_select_gpio(FLOW_METER_IN_GPIO);
    gpio_set_direction(FLOW_METER_IN_GPIO, GPIO_MODE_INPUT);
    gpio_pulldown_en(FLOW_METER_IN_GPIO);

    esp_rom_gpio_pad_select_gpio(VALVE_CTRL_OUT_GPIO);
    gpio_set_direction(VALVE_CTRL_OUT_GPIO, GPIO_MODE_INPUT_OUTPUT);
    // gpio_pulldown_en(VALVE_CTRL_OUT_GPIO);
    gpio_set_level(VALVE_CTRL_OUT_GPIO, 1);

    esp_rom_gpio_pad_select_gpio(LED_RED_OUT_GPIO);
    gpio_set_direction(LED_RED_OUT_GPIO, GPIO_MODE_INPUT_OUTPUT);
    gpio_set_level(LED_RED_OUT_GPIO, 0);

    esp_rom_gpio_pad_select_gpio(LED_BLUE_OUT_GPIO);
    gpio_set_direction(LED_BLUE_OUT_GPIO, GPIO_MODE_INPUT_OUTPUT);
    gpio_set_level(LED_BLUE_OUT_GPIO, 0);
}

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
    send_state(close ? CLOSING : OPENING);
}



