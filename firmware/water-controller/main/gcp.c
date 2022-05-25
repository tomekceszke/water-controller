#include <stdio.h>
#include <string.h>
#include <esp_log.h>
#include <time.h>
#include <esp_http_client.h>
#include "cJSON.h"
#include "config.h"
#include "common.h"
#include "base64url.h"

static const char *TAG = "GCP";
extern const uint8_t root_ca_cert_pem_start[] asm("_binary_gcp_ca_cert_pem_start");
extern const uint8_t root_ca_cert_pem_end[] asm("_binary_gcp_ca_cert_pem_end");

char *bool2string(_Bool b);

jwt_t createGCPJWT(time_t now);

jwt_t jwt;

const char *stateValues[] = {"START_RUNNING", "STOP_RUNNING", "OPENING", "CLOSING"};

esp_http_client_handle_t prepare_client(esp_http_client_method_t method, const char *url) {
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
        ESP_LOGE(TAG, "Couldn't re/create JWT token.");
    }

    esp_http_client_config_t config = {
            .url = url,
            .cert_pem = (char *) root_ca_cert_pem_start,
            .method = method,
            //.user_data = response_buffer,
            .buffer_size_tx = 2048,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);

    char authorization_header[1510];
    sprintf(authorization_header, "Bearer %s", jwt.payload);

    esp_http_client_set_header(client, "Authorization", authorization_header);
    esp_http_client_set_header(client, "Cache-Control", "no-cache");
    esp_http_client_set_header(client, "Content-Type", "application/json");

    return client;
}

void get(const char *url) {
    //char response_buffer[MAX_HTTP_OUTPUT_BUFFER] = {0};

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
        ESP_LOGE(TAG, "Couldn't re/create JWT token.");
    }

    esp_http_client_config_t config = {
            .url = url,
            .cert_pem = (char *) root_ca_cert_pem_start,
            .method = HTTP_METHOD_GET,
            //.user_data = response_buffer,
            .buffer_size_tx = BUFFER_SIZE_TX,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);

    char authorization_header[1510];
    sprintf(authorization_header, "Bearer %s", jwt.payload);
    ESP_LOGI(TAG, "%s", authorization_header); // TODO: remove

    esp_http_client_set_header(client, "Authorization", authorization_header);
    esp_http_client_set_header(client, "Cache-Control", "no-cache");
    esp_http_client_set_header(client, "Content-Type", "application/json");

    char *buffer = malloc(MAX_HTTP_RECV_BUFFER + 1);
    if (buffer == NULL) {
        ESP_LOGE(TAG, "Cannot malloc http receive buffer");
        return;
    }
    esp_err_t err;
    if ((err = esp_http_client_open(client, 0)) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open HTTP connection: %s", esp_err_to_name(err));
        free(buffer);
        return;
    }

    int content_length =  esp_http_client_fetch_headers(client);
    int total_read_len = 0, read_len;
    if (total_read_len < content_length && content_length <= MAX_HTTP_RECV_BUFFER) {
        read_len = esp_http_client_read(client, buffer, content_length);
        if (read_len <= 0) {
            ESP_LOGE(TAG, "Error read data");
        }
        buffer[read_len] = 0;
        ESP_LOGI(TAG, "read_len = %d", read_len);
        ESP_LOGI(TAG, "read = %s", buffer);
    }
    ESP_LOGI(TAG, "HTTP Stream reader Status = %d, content_length = %lld",
             esp_http_client_get_status_code(client),
             esp_http_client_get_content_length(client));
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    free(buffer);




//    esp_err_t err = esp_http_client_perform(client);
//
//    if (err == ESP_OK) {
//        ESP_LOGI(TAG, "HTTP GET %s request success (status: %d, content length: %lld)", url,  esp_http_client_get_status_code(client), esp_http_client_get_content_length(client));
//        ESP_LOGI(TAG, "Response length: %d", strlen(response_buffer));
//        ESP_LOG_BUFFER_HEX(TAG, response_buffer, strlen(response_buffer));
//    } else {
//        ESP_LOGE(TAG, "HTTP GET request failed: %s", esp_err_to_name(err));
//    }
//    esp_http_client_close(client);
//    esp_http_client_cleanup(client);
}


void post(const char *url, char *data) {
    esp_http_client_handle_t client = prepare_client(HTTP_METHOD_POST, url);
    esp_http_client_set_post_field(client, data, strlen(data));
    esp_err_t err = esp_http_client_perform(client);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP POST request failed: %s", esp_err_to_name(err));
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
}

void send_state(enum State state) {
    char data[50];
    sprintf(data, "{\"state\": \"%s\"}", stateValues[state]);

    ESP_LOGI(TAG, "%s", data);

    char binary_data[100];
    base64url_encode((unsigned char *) data, strlen(data), binary_data);

    char state_data[120];
    sprintf(state_data, "{\"binary_data\": \"%s\"}", binary_data);

    char post_data[150];
    sprintf(post_data, "{\"state\": %s}", state_data);

    post(GCP_PUBLISH_STATE_URL, post_data);
}

void send_event(int consumption, time_t start_time, time_t stop_time) {
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

    post(GCP_PUBLISH_EVENT_URL, post_data);
}

int get_config(int local_version) {
    ESP_LOGI(TAG, "Asking for config for local_version=%d", local_version);
    int remote_version = -1;
    char url[200];
    sprintf(url, GCP_GET_CONFIG_URL"?local_version=%d", local_version);

    //char response_buffer[MAX_HTTP_OUTPUT_BUFFER] = {0};

    get(url);
    
//    ESP_LOGI(TAG, "New config available (length: %d):", strlen(response_buffer));
//    ESP_LOG_BUFFER_HEX(TAG, response_buffer, strlen(response_buffer));
//    ESP_LOGI(TAG, "Parsing config...");
//    cJSON *root = cJSON_Parse(response_buffer);
//    ESP_LOGI(TAG, "Config parsed");
//
//    cJSON *version = cJSON_GetObjectItem(root, "version");
//    if(version) {
//        char *remote_version_string = version->valuestring;
//        ESP_LOGI(TAG, "Remote version: %s", remote_version_string);
//        remote_version = atoi(remote_version_string);
//    }
//
//    cJSON *binaryData = cJSON_GetObjectItem(root, "binaryData");
//    if(binaryData) {
//        char *binary_data = binaryData->valuestring;
//        ESP_LOGI(TAG, "Binary data:");
//        ESP_LOG_BUFFER_HEX(TAG, binary_data, strlen(binary_data));
//        unsigned char output_data[300];
//        base64url_decode(binary_data, strlen(binary_data), output_data);
//        ESP_LOGI(TAG, "Output data: %s", output_data);
//    }
//    cJSON *root_data = cJSON_Parse(output_data);
//    cJSON_GetObjectItem(root_data,"state")->valuestring;
    return remote_version;
}

