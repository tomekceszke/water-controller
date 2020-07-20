#include <stdint.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include <freertos/task.h>
#include <driver/ledc.h>
#include <time.h>
#include "driver/pcnt.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "config.h"

#define PCNT_TEST_UNIT      PCNT_UNIT_0
#define LEDC_OUTPUT_IO      18 // Output GPIO of a sample 1 Hz pulse generator

static const char *TAG = "COUNTER";

void led(gpio_num_t pin, bool on);

void close_valve(bool close);

void send_event(int, time_t, time_t);

#ifdef TEST_HW
static void ledc_init(void) {
    // Prepare and then apply the LEDC PWM timer configuration
    ledc_timer_config_t ledc_timer;
    ledc_timer.speed_mode = LEDC_LOW_SPEED_MODE;
    ledc_timer.timer_num = LEDC_TIMER_1;
    ledc_timer.duty_resolution = LEDC_TIMER_10_BIT;
    ledc_timer.freq_hz = 10;
    ledc_timer.clk_cfg = LEDC_AUTO_CLK;
    ledc_timer_config(&ledc_timer);

    // Prepare and then apply the LEDC PWM channel configuration
    ledc_channel_config_t ledc_channel;
    ledc_channel.speed_mode = LEDC_LOW_SPEED_MODE;
    ledc_channel.channel = LEDC_CHANNEL_1;
    ledc_channel.timer_sel = LEDC_TIMER_1;
    ledc_channel.intr_type = LEDC_INTR_DISABLE;
    ledc_channel.gpio_num = LEDC_OUTPUT_IO;
    ledc_channel.duty = 100; // set duty at about 10%
    ledc_channel.hpoint = 0;
    ledc_channel_config(&ledc_channel);
}
#endif

static void pcnt_init(void) {
    pcnt_config_t pcnt_config = {
            .pulse_gpio_num = FLOW_METER_IN_GPIO,
            .channel = PCNT_CHANNEL_0,
            .unit = PCNT_TEST_UNIT,
            .pos_mode = PCNT_COUNT_INC,
            .neg_mode = PCNT_COUNT_DIS,
    };

    pcnt_unit_config(&pcnt_config);

    //set a filter value to ignore pulses for an amount of clock cycles
    //ignore 10 clock cycles = 125ns. each cycle is 12.5ns based on 80Mhz APB clock.
    //filter_val is a 10-bit value, so the maximum filter_val should be limited to 1023.
    //the pulse duration should be longer than one APB_CLK cycle (12.5 ns)
    pcnt_set_filter_value(PCNT_TEST_UNIT, 100);
    pcnt_filter_enable(PCNT_TEST_UNIT);
    //pcnt_filter_disable(PCNT_TEST_UNIT);

    pcnt_counter_pause(PCNT_TEST_UNIT);
    pcnt_counter_clear(PCNT_TEST_UNIT);

    pcnt_counter_resume(PCNT_TEST_UNIT);
}

_Noreturn void pcnt_task() {
#ifdef TEST_HW
    ledc_init();
#endif
    pcnt_init();
    int16_t pulse_counter = 0;
    int16_t old_pulse_counter = 0;

    int sum_delta = 0;
    time_t start_time = 0;
    time_t stop_time = 0;

    bool running = false;
    bool flow_change = false;

    while (1) {
        // basic check
        if(running) {
            long duration = time(NULL) - start_time;
            if (duration > CUTOFF_SECONDS) {
                ESP_LOGE(TAG, "CUTOFF AFTER %lu seconds of flowing - FLOOD ALERT!", duration);
                close_valve(true);
                vTaskDelay((VALVE_CLOSING_TIME_SECONDS * 1000) / portTICK_RATE_MS);
            }
        }

        pcnt_get_counter_value(PCNT_TEST_UNIT, &pulse_counter);

        if (pulse_counter != old_pulse_counter) {
            int16_t delta = abs(pulse_counter - old_pulse_counter);
            // ESP_LOGI(TAG, "Delta %d", delta);
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
                //ESP_LOGW(TAG, "Water flow started!");
            } else {
                //ESP_LOGW(TAG, "Water flow stopped!");
                time(&stop_time);
                if(sum_delta > SEND_EVENT_THRESHOLD_PULSES || (stop_time-start_time) > SEND_EVENT_THRESHOLD_SECONDS ) { //TODO: aggregate somehow those short pulses
                    send_event(sum_delta, start_time, stop_time);
                }
                sum_delta = 0;
                start_time = 0;
                stop_time = 0;
            }
        }

        vTaskDelay(1000 / portTICK_RATE_MS);
    }
}
