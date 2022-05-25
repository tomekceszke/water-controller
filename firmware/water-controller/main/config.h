#include <driver/gpio.h>

#define TEST
//#define TEST_HW

#define VERSION                         __DATE__

/* Core */
#define WIFI_MAXIMUM_RETRY              10
#define NTP_MAX_ATTEMPTS                10

/* WEB Interface */
#define PORT_WEB                        80
#define SU_URL                          "https://192.168.11.15:8070/water-controller.bin"

/* GPIO */
#define FLOW_METER_IN_GPIO              GPIO_NUM_4
#define VALVE_CTRL_OUT_GPIO             GPIO_NUM_14
#define LED_BLUE_OUT_GPIO               GPIO_NUM_32
#define LED_RED_OUT_GPIO                GPIO_NUM_33

/* CUTOFF */
#define CUTOFF_SECONDS                  1200
#define VALVE_CLOSING_TIME_SECONDS      10

/* CLOUD */
#define GCP_PROJECT_ID                  "water-controller-351109"
#define GCP_PUBLISH_EVENT_URL           "https://cloudiotdevice.googleapis.com/v1/projects/water-controller-351109/locations/europe-west1/registries/water-controller-iot-registry/devices/water-controller-esp32-main:publishEvent"
#define GCP_PUBLISH_STATE_URL           "https://cloudiotdevice.googleapis.com/v1/projects/water-controller-351109/locations/europe-west1/registries/water-controller-iot-registry/devices/water-controller-esp32-main:setState"
#define GCP_GET_CONFIG_URL              "https://cloudiotdevice.googleapis.com/v1/projects/water-controller-351109/locations/europe-west1/registries/water-controller-iot-registry/devices/water-controller-esp32-main/config"
#define JWT_EXPIRY_SECONDS              600
#define JWT_EXPIRY_GAP_SECONDS          30
#define SEND_EVENT_THRESHOLD_PULSES     10
#define SEND_EVENT_THRESHOLD_SECONDS    1
#define MAX_HTTP_OUTPUT_BUFFER          512
#define MAX_HTTP_RECV_BUFFER            512
#define BUFFER_SIZE_TX                  2048
