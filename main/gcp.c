#include <stdio.h>
#include <string.h>
#include <esp_log.h>
#include <time.h>
#include <esp_http_client.h>
#include "config.h"
#include "base64url.h"

static const char *TAG = "GCP";
extern const uint8_t root_ca_cert_pem_start[] asm("_binary_root_ca_cert_pem_start");
//extern const uint8_t root_ca_cert_pem_end[] asm("_binary_root_ca_cert_pem_end");

jwt_t createGCPJWT();

jwt_t jwt;

void send_event(int consumption, time_t start_time, time_t stop_time) {
    if (jwt.payload == NULL) {
        jwt = createGCPJWT();
    } else {
        time_t now;
        time(&now);

        if (jwt.exp <= now) {
            ESP_LOGI(TAG, "JWT token is expired (exp: %d vs now: %lu)", jwt.exp, now);
            jwt = createGCPJWT();
        } else {
            ESP_LOGD(TAG, "Reused old JWT token that is valid to %d", jwt.exp);
        }
    }

    esp_http_client_config_t config = {
            .url = GCP_URL,
            .cert_pem = (char *) root_ca_cert_pem_start,
            .method = HTTP_METHOD_POST,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);

    char *authorization_header = (char *) malloc(strlen((char *) jwt.payload) + 8);
    sprintf(authorization_header, "Bearer %s", jwt.payload);

    esp_http_client_set_header(client, "Authorization", authorization_header);
    esp_http_client_set_header(client, "Content-Type", "application/json");

    char data[150];

    sprintf(data,
            "{\"start_time\":\"%lu\",\"stop_time\":\"%lu\",\"consumption\":\"%d\"}",
            start_time, stop_time, consumption);

    ESP_LOGI(TAG, "Data: %s", data);

    char binary_data[200];
    base64url_encode((unsigned char *) data, strlen(data), binary_data);

    char post_data[250];
    sprintf(post_data, "{\"binary_data\": \"%s\"}", binary_data);

    esp_http_client_set_post_field(client, post_data, strlen(post_data));

    esp_err_t err = esp_http_client_perform(client);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP POST request failed: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
}