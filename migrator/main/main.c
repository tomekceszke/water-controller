#include "driver/gpio.h"
#include "esp_log.h"

#include "hi_log.h"
#include "hi_migrator.h"
#include "hi_ota.h"
#include "hi_system.h"
#include "hi_wifi.h"

#include "blobs_manifest.h"
#include "config/config.h"
#include "config/credentials.h"

extern const uint8_t bl_start[] asm("_binary_bootloader_bin_start");
extern const uint8_t bl_end[] asm("_binary_bootloader_bin_end");
extern const uint8_t pt_start[] asm("_binary_partition_table_bin_start");
extern const uint8_t pt_end[] asm("_binary_partition_table_bin_end");
extern const char ota_cert_pem_start[] asm("_binary_ota_server_cert_15_pem_start");

static const uint8_t BL_SHA256[32] = BLOB_BOOTLOADER_SHA256;
static const uint8_t PT_SHA256[32] = BLOB_PARTITION_TABLE_SHA256;

void app_main(void)
{
    // Valve: the same level the legacy firmware drives at every boot (open). Firmware 3.x keeps it when it takes over.
    gpio_set_level(GPIO_VALVE, VALVE_LEVEL_OPEN);
    gpio_set_direction(GPIO_VALVE, GPIO_MODE_INPUT_OUTPUT);
    gpio_set_level(GPIO_VALVE, VALVE_LEVEL_OPEN);
    gpio_set_direction(GPIO_LED_FLOW, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_LED_FLOW, 1);           // both LEDs on: migrator running
    gpio_set_direction(GPIO_LED_CLOSED, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_LED_CLOSED, 1);

    hi_nvs_init(NULL);
    hi_log_init(&(hi_log_config_t) {.udp_ip = LOG_UDP_IP, .udp_port = LOG_UDP_PORT});
    hi_wifi_start(&(hi_wifi_config_t) {.ssid = WIFI_SSID, .password = WIFI_PASS, .hostname = DEVICE_HOSTNAME});
    hi_ota_init(&(hi_ota_config_t) {.url = OTA_URL, .cert_pem = ota_cert_pem_start, .delete_after = true});
    hi_wifi_wait_connected(60000);

    hi_migrator_run(&(hi_migrator_config_t) {
        .name = "water-migrator",
        .bootloader = bl_start,
        .bootloader_len = bl_end - bl_start,
        .bootloader_sha256 = BL_SHA256,
        .partition_table = pt_start,
        .partition_table_len = pt_end - pt_start,
        .partition_table_sha256 = PT_SHA256,
        .ota1_offset = 0x210000,
        .admin_header_value = HEADER_AUTHORIZATION_VALUE,
    });
}
