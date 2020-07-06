#include <driver/gpio.h>
#include "credentials.h"

#define TEST
//#define TEST_HW

#define VERSION                         __DATE__

#define PORT_WEB                        80
#define SU_URL                          "https://192.168.11.90:8070/water-controller.bin"

#define FLOW_METER_IN_GPIO              GPIO_NUM_4
#define VALVE_CTRL_OUT_GPIO             GPIO_NUM_14
#define LED_BLUE_OUT_GPIO               GPIO_NUM_32
#define LED_RED_OUT_GPIO                GPIO_NUM_33

#define VALVE_CLOSING_TIME_SECONDS      8


#define CUTOFF_SECONDS                  1800 // TODO

typedef struct {
    char *payload;
    uint32_t exp;
} jwt_t;

#define PROJECT_ID                      "vps1-ceszke-com"
#define GCP_URL                         "https://cloudiotdevice.googleapis.com/v1/projects/vps1-ceszke-com/locations/europe-west1/registries/vps1-iot/devices/water-controller:publishEvent"
