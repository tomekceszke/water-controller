#pragma once

#include "driver/gpio.h"

/* DEVICE */
#define DEVICE_HOSTNAME                 "water-controller"

/* GPIO */
#define GPIO_FLOW_METER                 GPIO_NUM_4      // PM-3/4-B open collector, rising edges
#define GPIO_VALVE                      GPIO_NUM_14     // HIGH = open; strapping pin MTMS
#define GPIO_LED_FLOW                   GPIO_NUM_32     // blue
#define GPIO_LED_CLOSED                 GPIO_NUM_33     // red
#define VALVE_LEVEL_OPEN                1

/* FLOW METER */
#define FLOW_SAMPLE_MS                  1000
#define FLOW_GLITCH_NS                  10000   // real pulses are >= 1.4 ms (45 L/min); PCNT maximum ~12.8 us
#define FLOW_DIAG_MAX_MIN               60      // pulse-interval diagnostics run at most this long
#define FLOW_TASK_CORE                  1       // WiFi/lwIP live on core 0
#define FLOW_TASK_PRIORITY              10

/* TIER 1 (defaults and hard bounds; the limit is set from the app) */
#define TIER1_LIMIT_DEFAULT_S           1200
#define TIER1_LIMIT_MIN_S               60
#define TIER1_LIMIT_MAX_S               (4 * 3600)
#define TIER1_GAP_MS                    2000    // a pause longer than this ends a continuous flow
#define VALVE_CLOSING_S                 15      // flow after a close for longer than this = valve failure alert

/* CALIBRATION */
#define PULSES_PER_LITER_DEFAULT        477     // PM-3/4-B datasheet, +-10 %: see docs/CALIBRATION.md
#define PULSES_PER_LITER_MIN            300
#define PULSES_PER_LITER_MAX            700

/* TIER 2 (all rules off by default) */
#define TIER2_MAX_LITERS_LIMIT          10000
#define TIER2_MAX_LPM_LIMIT             60
#define TIER2_MAX_BURST_S               3600
#define TIER2_MAX_LEAK_MIN              (24 * 60)
#define TIER2_MAX_SNOOZE_MIN            (12 * 60)
#define PROTECT_TASK_PRIORITY           5
#define PROTECT_QUEUE_LEN               120     // samples; drops (never blocks Tier 1) when Tier 2 lags

/* EVENTS */
#define EVENTS_RING_SIZE                50

/* TELEMETRY (hc-data; failures never affect protection) */
#define MQTT_BROKER_URI                 "mqtt://192.168.11.16:1883"
#define MQTT_TOPIC_PREFIX               "water"
#define MQTT_OUTBOX_LIMIT_BYTES         32768
#define TELEMETRY_QUEUE_LEN             64
#define TELEMETRY_SAMPLE_PERIOD_S       10      // flow samples published while water flows

/* OTA (rehearsal builds for the spare board use another file name, so production never fetches them:
 * -DWATER_OTA_FILE=water-controller-spare.bin) */
#ifndef OTA_FILE
#define OTA_FILE                        "water-controller.bin"
#endif
#define OTA_URL                         "https://192.168.11.15:8070/" OTA_FILE

/* LOGGING */
#define LOG_UDP_IP                      "192.168.11.15"
#define LOG_UDP_PORT                    1338

/* HTTPD */
#define HTTPD_ALLOWED_HOSTS             { DEVICE_HOSTNAME, DEVICE_HOSTNAME ".lan" }
#define APP_URL                         "http://192.168.11.244/"

/* HEALTH */
#define HEALTH_VERIFY_TIMEOUT_S         300     // new image: Tier 1 alive + WiFi within this, else rollback
#define HEALTH_MAX_UNVERIFIED_BOOTS     3
#define HEALTH_STATS_LOG_PERIOD_S       900

/* NOTIFY */
#define NOTIFY_ERROR_COOLDOWN_S         3600
