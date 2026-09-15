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

char *createGCPJWT(time_t now, time_t exp, char *target_url);

token_t token;

char *parse_id_token(char *data) {
    cJSON *root = cJSON_Parse(data);
    cJSON *id_token = cJSON_GetObjectItem(root, "id_token");
    if (id_token) {
        char *id_token_string = id_token->valuestring;
        // ESP_LOGI(TAG, "Found id_token: %s", id_token_string);
        return id_token_string;
    }
    ESP_LOGE(TAG, "No id_token object found");
    return NULL;
}


char *obtain_id_token(time_t now, time_t exp, char *target_url) {
    char *jwt = createGCPJWT(now, exp, target_url);

    if (jwt == NULL) {
        ESP_LOGE(TAG, "Cannot obtain id token");
        return NULL;
    }

    esp_err_t err;
    char output_buffer[MAX_HTTP_OUTPUT_BUFFER + 1] = {0};
    esp_http_client_config_t config = {
            .url = JWT_GCP_TOKEN_URL,
            .cert_pem = (char *) root_ca_cert_pem_start,
            .method = HTTP_METHOD_POST,
            .buffer_size_tx = BUFFER_SIZE_TX,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);

    char authorization_header[1000];
    sprintf(authorization_header, "Bearer %s", jwt);
    // ESP_LOGI(TAG, "Authorization header to obtain id token: %s, len: %d", authorization_header, strlen(authorization_header));

    char post_data[1000];
    sprintf(post_data, "grant_type=urn:ietf:params:oauth:grant-type:jwt-bearer&assertion=%s", jwt);
    // ESP_LOGI(TAG, "Payload to obtain id token: %s, len: %d", post_data, strlen(post_data));

    esp_http_client_set_url(client, JWT_GCP_TOKEN_URL);
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Authorization", authorization_header);
    //esp_http_client_set_header(client, "Cache-Control", "no-cache");
    esp_http_client_set_header(client, "Content-Type", "application/x-www-form-urlencoded");

    err = esp_http_client_open(client, strlen(post_data));

    char *id_token = NULL;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open HTTP connection: %s", esp_err_to_name(err));
    } else {
        int wlen = esp_http_client_write(client, post_data, strlen(post_data));
        if (wlen < 0) {
            ESP_LOGE(TAG, "Write failed");
        }
        int64_t content_length = esp_http_client_fetch_headers(client);
        if (content_length < 0) {
            ESP_LOGE(TAG, "HTTP client fetch headers failed");
        } else {
            int data_read = esp_http_client_read_response(client, output_buffer, MAX_HTTP_OUTPUT_BUFFER);
            if (data_read > 0) {
                // ESP_LOGI(TAG, "HTTP POST Status = %d", esp_http_client_get_status_code(client));
                id_token = parse_id_token(output_buffer);
            } else {
                ESP_LOGE(TAG, "Failed to read response");
            }
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return id_token;
}

char *get_token(char *url) {
    time_t now;
    time(&now);
    time_t curr_exp = now + JWT_EXPIRY_SECONDS;

    if (token.payload == NULL) {
        ESP_LOGI(TAG, "Creating token for the first time (curr_exp: %lld vs now: %lld)", curr_exp, now);
        token.payload = obtain_id_token(now, curr_exp, url);
        token.exp = curr_exp;
    } else if (token.exp <= (now + JWT_EXPIRY_GAP_SECONDS)) {
        ESP_LOGI(TAG, "The token has expired (exp: %lu vs now: %lld), creating the new one", token.exp,
                 (now + JWT_EXPIRY_GAP_SECONDS));
        token.payload = obtain_id_token(now, curr_exp, url);
        token.exp = curr_exp;
    } else {
        // ESP_LOGI(TAG, "Using already generated token (exp: %lu vs now: %lld)", token.exp, now);
    }
    return token.payload;
}


esp_http_client_handle_t prepare_client(esp_http_client_method_t method, char *url) {
    esp_http_client_config_t config = {
            .url = url,
            .cert_pem = (char *) root_ca_cert_pem_start,
            .method = method,
            .buffer_size_tx = BUFFER_SIZE_TX,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);

    char *id_token = get_token(url);
    char authorization_header[1000];
    sprintf(authorization_header, "Bearer %s", id_token);
    //ESP_LOGI(TAG, "Authorization header: %s", authorization_header);

    esp_http_client_set_header(client, "Authorization", authorization_header);
    //esp_http_client_set_header(client, "Cache-Control", "no-cache");
    esp_http_client_set_header(client, "Content-Type", "application/json");

    return client;
}

char *get(char *url) {
    static char buffer[MAX_HTTP_RECV_BUFFER + 1];
    esp_err_t err;
    esp_http_client_handle_t client = prepare_client(HTTP_METHOD_GET, url);
    if ((err = esp_http_client_open(client, 0)) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open HTTP connection: %s", esp_err_to_name(err));
    } else {

        int64_t content_length = esp_http_client_fetch_headers(client);
        if (content_length < 0) {
            ESP_LOGE(TAG, "HTTP client fetch headers failed");
        } else {
            int data_read = esp_http_client_read_response(client, buffer, MAX_HTTP_OUTPUT_BUFFER);
            if (data_read <= 0) {
                ESP_LOGW(TAG, "Failed to read response");
            }
        }
        ESP_LOGI(TAG, "HTTP url: %s,  status: %d", url, esp_http_client_get_status_code(client));
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return buffer;

//    esp_err_t err = esp_http_client_perform(client);
//
//    if (err == ESP_OK) {
//        ESP_LOGI(TAG, "HTTP GET %s request success (status: %d, content length: %lld)", url,  esp_http_client_get_status_code(client), esp_http_client_get_content_length(client));
//        ESP_LOGI(TAG, "Response length: %d", strlen(response_buffer));
//        ESP_LOG_BUFFER_HEX(TAG, response_buffer, strlen(response_buffer));
//    } else {
//        ESP_LOGE(TAG, "HTTP GET request failed: %s", esp_err_to_name(err));
//    }
}


void post(char *url, char *data) {
    esp_http_client_handle_t client = prepare_client(HTTP_METHOD_POST, url);
    esp_http_client_set_post_field(client, data, (int) strlen(data));
    esp_err_t err = esp_http_client_perform(client);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP POST request failed: %s", esp_err_to_name(err));
    } else {
        // ESP_LOGI(TAG, "Sent OK");
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
}

void send_metrics(int consumption, time_t start_time, time_t stop_time) {
    char data[150];
    sprintf(data, "{\"start_time\":%lld,\"stop_time\":%lld,\"consumption\":%d}",
            start_time, stop_time, consumption);
    ESP_LOGI(TAG, "%s", data);
    post(GCP_SEND_METRICS_URL, data);
}

