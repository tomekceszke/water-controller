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

jwt_t createGCPJWT(time_t now);

jwt_t jwt;

void send_event(int consumption, time_t start_time, time_t stop_time) {
    time_t now;
    time(&now);

    if (strlen(jwt.payload) == 0) {
        jwt = createGCPJWT(now);
    }

    if (jwt.exp <= (now + JWT_EXPIRY_GAP_SECONDS)) {
        // ESP_LOGI(TAG, "JWT token is expired (exp: %d vs now: %lu)", jwt.exp, (now + JWT_EXPIRY_GAP_SECONDS));
        jwt = createGCPJWT(now);
    } else {
        // ESP_LOGI(TAG, "Using already generated JWT token (exp: %d vs now: %lu)", jwt.exp, now);
    }

    if (strlen(jwt.payload) == 0) {
        ESP_LOGE(TAG, "Couldn't re/create JWT token. Event won't be sent.");
        return;
    }


    esp_http_client_config_t config = {
            .url = GCP_PUBLISH_EVENT_URL,
            .cert_pem = (char *) root_ca_cert_pem_start,
            .method = HTTP_METHOD_POST,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);

    char authorization_header[1510];
    sprintf(authorization_header, "Bearer %s", jwt.payload);

    esp_http_client_set_header(client, "Authorization", authorization_header);
    esp_http_client_set_header(client, "Content-Type", "application/json");

    char data[150];
    sprintf(data,
            "{\"start_time\":\"%lu\",\"stop_time\":\"%lu\",\"consumption\":\"%d\"}",
            start_time, stop_time, consumption);

    //ESP_LOGI(TAG, "Authorization header size: %d", strlen(authorization_header));
    ESP_LOGI(TAG, "%s", data);

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