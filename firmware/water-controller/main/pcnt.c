#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include <freertos/task.h>
#include <time.h>
#include "driver/pulse_cnt.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "config.h"
#include "common.h"


static const char *TAG = "COUNTER";

bool running = false;
time_t last_start_time;
time_t last_stop_time;
int last_consumption;


void led(gpio_num_t pin, bool on);

void close_valve(bool close);

//void send_state(enum State state);

void send_metrics(int consumption, time_t start_time, time_t stop_time);


static void pcnt_init(pcnt_unit_handle_t* pcnt_unit) {
    ESP_LOGD(TAG, "Install PCNT unit");
    pcnt_unit_config_t unit_config = {
            .high_limit = INT16_MAX,
            .low_limit = -1
    };

    ESP_ERROR_CHECK(pcnt_new_unit(&unit_config, pcnt_unit));

    ESP_LOGD(TAG, "Set glitch filter");
    pcnt_glitch_filter_config_t filter_config = {
            .max_glitch_ns = PCNT_MAX_GLITCH_NS,
    };
    ESP_ERROR_CHECK(pcnt_unit_set_glitch_filter(*pcnt_unit, &filter_config));

    ESP_LOGD(TAG, "Install PCNT channel");
    pcnt_chan_config_t chan_config = {
            .edge_gpio_num = FLOW_METER_IN_GPIO,
            .level_gpio_num = -1,
    };
    pcnt_channel_handle_t pcnt_chan = NULL;
    ESP_ERROR_CHECK(pcnt_new_channel(*pcnt_unit, &chan_config, &pcnt_chan));

    ESP_LOGD(TAG, "Set edge and level actions for PCNT channel");
    ESP_ERROR_CHECK(pcnt_channel_set_edge_action(pcnt_chan, PCNT_CHANNEL_EDGE_ACTION_INCREASE, PCNT_CHANNEL_EDGE_ACTION_HOLD));

    ESP_LOGD(TAG, "Enable PCNT unit");
    ESP_ERROR_CHECK(pcnt_unit_enable(*pcnt_unit));

    ESP_LOGD(TAG, "Clear PCNT unit");
    ESP_ERROR_CHECK(pcnt_unit_clear_count(*pcnt_unit));

    ESP_LOGD(TAG, "Start PCNT unit");
    ESP_ERROR_CHECK(pcnt_unit_start(*pcnt_unit));
    ESP_LOGI(TAG, "PCNT unit started");
}

_Noreturn void pcnt() {
    pcnt_unit_handle_t pcnt_unit = NULL;
    pcnt_init(&pcnt_unit);
    int pulse_counter = 0;
    int old_pulse_counter = 0;
    int sum_delta = 0;
    // int aggregate_counter = 0;
    time_t start_time = 0;
    time_t stop_time = 0;
    bool flow_change = false;

    while (1) {
        // basic check
        if (running) {
            time_t duration = time(NULL) - start_time;
            if (duration > CUTOFF_SECONDS) {
                ESP_LOGE(TAG, "CUTOFF AFTER %lld seconds of flowing - FLOOD ALERT!", duration);
                close_valve(true);
                vTaskDelay((VALVE_CLOSING_TIME_SECONDS * 1000) / portTICK_PERIOD_MS);
            }
        } // TODO: add counter overflow INT16_MAX case

        ESP_ERROR_CHECK(pcnt_unit_get_count(pcnt_unit, &pulse_counter));

        if (pulse_counter != old_pulse_counter) {
            int delta = abs(pulse_counter - old_pulse_counter);
            ESP_LOGD(TAG, "Delta %d", delta);
            sum_delta += delta;
            old_pulse_counter = pulse_counter;
            flow_change = !running;
            running = true;
        } else {
            flow_change = running;
            running = false;
        }

        if (flow_change) {
            led(LED_BLUE_OUT_GPIO, running);
            if (running) {
                time(&start_time);
                last_start_time = start_time;
                //send_state(START_RUNNING);
                //ESP_LOGI(TAG, "Water flow started!");
            } else {
                //ESP_LOGI(TAG, "Water flow stopped!");
                time(&stop_time);
                last_stop_time = stop_time;
                time_t duration = stop_time - start_time;
                last_consumption = sum_delta;

                ESP_LOGI(TAG, "Detected flow: consumption: %d, duration: %lld", sum_delta, duration);
                //ESP_LOGI(TAG, "Free heap size: %lu", xPortGetFreeHeapSize());

                if (sum_delta > SEND_METRICS_THRESHOLD_PULSES && duration > SEND_METRICS_THRESHOLD_SECONDS) {
                    send_metrics(sum_delta, start_time, stop_time);
                }
                sum_delta = 0;
                start_time = 0;
                stop_time = 0;
            }
        }
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}
