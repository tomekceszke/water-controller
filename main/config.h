#include <driver/gpio.h>
#include "credentials.h"

#define TEST
//#define TEST_HW

#define VERSION                         __DATE__


/* WEB Interface */
#define PORT_WEB                        80
#define SU_URL                          "https://192.168.11.90:8070/water-controller.bin"

/* GPIO */
#define FLOW_METER_IN_GPIO              GPIO_NUM_4
#define VALVE_CTRL_OUT_GPIO             GPIO_NUM_14
#define LED_BLUE_OUT_GPIO               GPIO_NUM_32
#define LED_RED_OUT_GPIO                GPIO_NUM_33

/* CUTOFF */
#define CUTOFF_SECONDS                  1800
#define VALVE_CLOSING_TIME_SECONDS      10

/* CLOUD */

typedef struct { // TODO move it out from here
    uint32_t exp;
    char payload[1500]; // TODO change to pointer, static or extern?
} jwt_t;


#define GCP_PROJECT_ID                  "vps1-ceszke-com"
#define GCP_PUBLISH_EVENT_URL           "https://cloudiotdevice.googleapis.com/v1/projects/vps1-ceszke-com/locations/europe-west1/registries/vps1-iot/devices/water-controller:publishEvent"
#define JWT_EXPIRY_SECONDS              600
#define JWT_EXPIRY_GAP_SECONDS          30
#define SEND_EVENT_THRESHOLD_PULSES     10
#define SEND_EVENT_THRESHOLD_SECONDS    1

#define NTP_MAX_ATTEMPTS                3