#include <esp_log.h>
#include <driver/gpio.h>
#include <time.h>
#include "esp_http_server.h"
#include "config.h"
#include "common.h"
#include "credentials.h"

static const char *HEADER_AUTHORIZATION_KEY = "Authorization";
static const char *TAG = "WEB";

extern const uint8_t index_html_gz_start[] asm("_binary_index_html_gz_start");
extern const uint8_t index_html_gz_end[]   asm("_binary_index_html_gz_end");

void ota();

void close_valve(bool close);

bool is_valve_closed();

jwt_t createGCPJWT(time_t now);

void send_state(enum State state);

void send_event(int, time_t, time_t);

int get_config(int);

char *bool2string(_Bool b );

extern char boot_time[64];

extern bool running;
extern time_t last_start_time;
extern time_t last_stop_time;
extern int last_consumption;


static bool auth(httpd_req_t *req) {
    char *auth_data;
    size_t auth_data_len = httpd_req_get_hdr_value_len(req, HEADER_AUTHORIZATION_KEY) + 1;

    if (auth_data_len > 1) {
        auth_data = (char *) malloc(auth_data_len);
        esp_err_t auth_data_header_err = httpd_req_get_hdr_value_str(req, HEADER_AUTHORIZATION_KEY, auth_data,
                                                                     auth_data_len);
        if (auth_data_header_err == ESP_OK) {
            if (strcmp(auth_data, HEADER_AUTHORIZATION_VALUE) == 0) {
                free(auth_data);
                ESP_LOGD(TAG, "Authorization successful!");
                return true;
            } else {
                ESP_LOGE(TAG, "Authorization invalid!");
            }
        } else if (auth_data_header_err == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Authorization invalid! ESP_ERR_NOT_FOUND");
        } else if (auth_data_header_err == ESP_ERR_INVALID_ARG) {
            ESP_LOGE(TAG, "Authorization invalid! ESP_ERR_INVALID_ARG");
        } else if (auth_data_header_err == ESP_ERR_HTTPD_INVALID_REQ) {
            ESP_LOGE(TAG, "Authorization invalid! ESP_ERR_HTTPD_INVALID_REQ");
        } else if (auth_data_header_err == ESP_ERR_HTTPD_RESULT_TRUNC) {
            ESP_LOGE(TAG, "Authorization invalid! ESP_ERR_HTTPD_RESULT_TRUNC");
        } else {
            ESP_LOGE(TAG, "Authorization invalid!");
        }
        free(auth_data);
    } else {
        ESP_LOGW(TAG, "Authorization header not found");
    }
    return false;
}

static esp_err_t doAuth(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"Secure\"");
    httpd_resp_set_status(req, "401 Unauthorized");
    return httpd_resp_send(req, "", 0);
}

static esp_err_t main_handler(httpd_req_t *req) {
    if (!auth(req)) {
        return doAuth(req);
    }
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_type(req, "text/html");

    //const char resp[] = "<html><body><h1>Water-controller</h1></body></html>";
    //return httpd_resp_send(req, resp, strlen(resp));
    return httpd_resp_send(req, (const char*)index_html_gz_start, (index_html_gz_end - index_html_gz_start));

}

static esp_err_t su_handler(httpd_req_t *req) {
    if (!auth(req)) {
        return doAuth(req);
    }
    int length = req->content_len;
    if (length == 0) {
        ESP_LOGE(TAG, "Empty content");
        return ESP_FAIL;
    }

    const char resp[] = "Upgrade in progres...";
    ota();
    //xTaskCreate(&ota_task, "ota_task", 8192, NULL, 5, NULL);

    return httpd_resp_send(req, resp, strlen(resp));
}

static esp_err_t reboot_handler(httpd_req_t *req) {
    if (!auth(req)) {
        return doAuth(req);
    }
    int length = req->content_len;
    if (length == 0) {
        ESP_LOGE(TAG, "Empty content");
        return ESP_FAIL;
    }
    ESP_LOGE(TAG, "(not error) Rebooting!");
    //const char resp[] = "Reboot in progres...";
    //xTaskCreate(&reboot_task, "reboot_task", 8192, NULL, 5, NULL);
    esp_restart();
    //return httpd_resp_send(req, resp, strlen(resp));
}

//static esp_err_t jwt_handler(httpd_req_t *req) {
//    if (!auth(req)) {
//        return doAuth(req);
//    }
//    int length = req->content_len;
//    if (length == 0) {
//        ESP_LOGE(TAG, "Empty content");
//        return ESP_FAIL;
//    }
//    jwt_t jwt = createGCPJWT();
//
//    return httpd_resp_send(req, jwt.payload, strlen(jwt.payload));
//}

static esp_err_t is_valve_closed_handler(httpd_req_t *req) {
    bool state = is_valve_closed();
    const char *state_str = state ? "1" : "0";
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, state_str, strlen(state_str));
}

static esp_err_t close_valve_handler(httpd_req_t *req) {
    if (!auth(req)) {
        return doAuth(req);
    }
    int length = req->content_len;
    if (length == 0) {
        ESP_LOGE(TAG, "Empty content");
        return ESP_FAIL;
    }

    char buf[length];
    httpd_req_recv(req, buf, length);
    int close;
    sscanf(buf, "%d", &close);
    close_valve(close == 1);

    return httpd_resp_send(req, "", strlen(""));
}

static esp_err_t get_hw_status_handler(httpd_req_t *req) {
    size_t free_bytes = xPortGetFreeHeapSize();
    //int64_t uptime = esp_timer_get_time();
    char data[100];
    sprintf(data,
            "{\"up_since\":\"%s\",\"free_kb\":\"%d\"}", boot_time, free_bytes/1024);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, data, strlen(data));
}

static esp_err_t get_config_handler(httpd_req_t *req) {
    if (!auth(req)) {
        return doAuth(req);
    }
    char*  buf;
    int version = 0;
    size_t buf_len = httpd_req_get_url_query_len(req) + 1;
    if (buf_len > 1) {
        buf = malloc(buf_len);
        if (httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK) {
            char param[32];
            /* Get value of expected key from query string */
            if (httpd_query_key_value(buf, "version", param, sizeof(param)) == ESP_OK) {
                version = atoi(param);
            }
        } else {
            ESP_LOGE(TAG, "httpd_req_get_url_query_str");
        }
        free(buf);
    }
    int remote_version = get_config(version);
    char data[30];
    sprintf(data, "Remote version: %d", remote_version);
    return httpd_resp_send(req, data, strlen(data));
}

static esp_err_t get_status_handler(httpd_req_t *req) {
    char data[100];
    sprintf(data,
            "{"
                "\"is_closed\":%s,"
                "\"is_running\":%s,"
                "\"start_time\":\"%ld\","
                "\"stop_time\":\"%ld\","
                "\"consumption\":\"%d\""
            "}",
            bool2string(is_valve_closed()),
            bool2string(running),
            last_start_time,
            last_stop_time,
            last_consumption);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, data, strlen(data));
}

static esp_err_t trigger_test_state_to_gcp_handler(httpd_req_t *req) {
    send_state(START_RUNNING);
    return httpd_resp_send(req, "", strlen(""));
}

static esp_err_t trigger_test_event_to_gcp_handler(httpd_req_t *req) {
    send_event(0, time(NULL), time(NULL) + 1);
    return httpd_resp_send(req, "", strlen(""));
}

void httpd(int port) {
    httpd_handle_t web_httpd = NULL;
    httpd_config_t http_config = HTTPD_DEFAULT_CONFIG();
    http_config.server_port = port;
    http_config.ctrl_port = port;
    http_config.lru_purge_enable = true;
    http_config.max_uri_handlers = 15;

    http_config.stack_size = 16384; // TODO!!!

    httpd_uri_t main_uri = {
            .uri = "/",
            .method = HTTP_GET,
            .handler = main_handler,
            .user_ctx = NULL};

    httpd_uri_t su_uri = {
            .uri = "/su",
            .method = HTTP_POST,
            .handler = su_handler,
            .user_ctx = NULL};

    httpd_uri_t reboot_uri = {
            .uri = "/reboot",
            .method = HTTP_POST,
            .handler = reboot_handler,
            .user_ctx = NULL};

//    httpd_uri_t jwt_uri = {
//            .uri = "/jwt",
//            .method = HTTP_POST,
//            .handler = jwt_handler,
//            .user_ctx = NULL};

    httpd_uri_t close_valve_uri = {
            .uri = "/close-valve",
            .method = HTTP_POST,
            .handler = close_valve_handler,
            .user_ctx = NULL};

    httpd_uri_t is_valve_closed_uri = {
            .uri = "/is-valve-closed",
            .method = HTTP_GET,
            .handler = is_valve_closed_handler,
            .user_ctx = NULL};

    httpd_uri_t trigger_test_state_to_gcp_uri = {
            .uri = "/trigger-gcp-state",
            .method = HTTP_GET,
            .handler = trigger_test_state_to_gcp_handler,
            .user_ctx = NULL};

    httpd_uri_t trigger_test_event_to_gcp_uri = {
            .uri = "/trigger-gcp-event",
            .method = HTTP_GET,
            .handler = trigger_test_event_to_gcp_handler,
            .user_ctx = NULL};

    httpd_uri_t get_hw_status_uri = {
            .uri = "/hw-status",
            .method = HTTP_GET,
            .handler = get_hw_status_handler,
            .user_ctx = NULL};

    httpd_uri_t get_config_uri = {
            .uri = "/get-config",
            .method = HTTP_GET,
            .handler = get_config_handler,
            .user_ctx = NULL};

    httpd_uri_t get_status_uri = {
            .uri = "/api/status",
            .method = HTTP_GET,
            .handler = get_status_handler,
            .user_ctx = NULL};


    ESP_LOGI(TAG, "Web server started on port: '%d'", http_config.server_port);

    if (httpd_start(&web_httpd, &http_config) == ESP_OK) {
        httpd_register_uri_handler(web_httpd, &main_uri);
        httpd_register_uri_handler(web_httpd, &su_uri);
        httpd_register_uri_handler(web_httpd, &reboot_uri);
        // httpd_register_uri_handler(web_httpd, &jwt_uri);
        httpd_register_uri_handler(web_httpd, &close_valve_uri);
        httpd_register_uri_handler(web_httpd, &is_valve_closed_uri);
        httpd_register_uri_handler(web_httpd, &get_hw_status_uri);
        httpd_register_uri_handler(web_httpd, &get_status_uri);
        httpd_register_uri_handler(web_httpd, &get_config_uri);
#ifdef TEST
        httpd_register_uri_handler(web_httpd, &trigger_test_state_to_gcp_uri);
        httpd_register_uri_handler(web_httpd, &trigger_test_event_to_gcp_uri);
#endif

    }

}