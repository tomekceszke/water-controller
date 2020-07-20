#include "config.h"
#include "esp_https_ota.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <esp_log.h>

static const char *TAG = "OTA";
extern const uint8_t server_cert_pem_start[] asm("_binary_ca_cert_pem_start");
extern const uint8_t server_cert_pem_end[] asm("_binary_ca_cert_pem_end");

esp_err_t init_nvs() {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}


esp_err_t _http_event_handler(esp_http_client_event_t *evt) {
    switch (evt->event_id) {
        case HTTP_EVENT_ERROR:
            ESP_LOGD(TAG, "HTTP_EVENT_ERROR");
            break;
        case HTTP_EVENT_ON_CONNECTED:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_CONNECTED");
            break;
        case HTTP_EVENT_HEADER_SENT:
            ESP_LOGD(TAG, "HTTP_EVENT_HEADER_SENT");
            break;
        case HTTP_EVENT_ON_HEADER:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
            break;
        case HTTP_EVENT_ON_DATA:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
            break;
        case HTTP_EVENT_ON_FINISH:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_FINISH");
            break;
        case HTTP_EVENT_DISCONNECTED:
            ESP_LOGD(TAG, "HTTP_EVENT_DISCONNECTED");
            break;
    }
    return ESP_OK;
}


esp_err_t remove_bin_file() {
    esp_http_client_config_t config = {
            .url = SU_URL,
            .cert_pem = (char *) server_cert_pem_start,
            .method = HTTP_METHOD_DELETE,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_err_t err = esp_http_client_perform(client);
    esp_http_client_cleanup(client);

    return err;
}

void ota_task() {
    ESP_LOGI(TAG, "Starting OTA...");
    ESP_ERROR_CHECK(init_nvs());
    //xTaskCreate(&ota_task, "boot_ota_task", 8192, (void *) 1, 5, NULL);

    esp_http_client_config_t config = {
            .url = SU_URL,
            .cert_pem = (char *) server_cert_pem_start,
            .event_handler = _http_event_handler,
    };

//#ifdef SKIP_COMMON_NAME_CHECK
//    config.skip_cert_common_name_check = true;
//#endif
    esp_err_t ret = esp_https_ota(&config);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Firmware upgraded");
        remove_bin_file();
//        if (remove_bin_file() == ESP_OK) {
//            ESP_LOGW(TAG, "Restarting...");
//            esp_restart();
//        } else {
//            ESP_LOGE(TAG, "Please remove manually BIN file and then restart ESP!");
//        }
        ESP_LOGW(TAG, "Restarting...");
        esp_restart();
    } else {
        ESP_LOGE(TAG, "Firmware upgrade failed");
    }
}
