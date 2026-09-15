/**
 * Original file: https://github.com/nkolban/esp32-snippets/blob/master/cloud/GCP/JWT/main.cpp
 * Original author: https://github.com/nkolban
 * License: https://github.com/nkolban/esp32-snippets/blob/master/LICENSE
 */

#include <stdio.h>
#include <string.h>
#include <mbedtls/pk.h>
#include <mbedtls/error.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <esp_log.h>

#include "base64url.h"
#include "common.h"
#include "config.h"

extern const uint8_t private_pem_start[] asm("_binary_water_controller_1e47_pem_start");
extern const uint8_t private_pem_end[] asm("_binary_water_controller_1e47_pem_end");

static const char *TAG = "JWT";


/**
 * Return a string representation of an mbedtls error code
 */
static char *mbedtlsError(int errnum) {
    static char buffer[200];
    mbedtls_strerror(errnum, buffer, sizeof(buffer));
    return buffer;
}

char *createGCPJWT(time_t now, time_t exp, char *target_url) {
    char base64Header[100];
    const char header[] = "{\"alg\":\"RS256\",\"typ\":\"JWT\"}";
    base64url_encode(
            (unsigned char *) header,   // Data to encode.
            strlen(header),            // Length of data to encode.
            base64Header);             // Base64 encoded data.


    char payload[500];
    sprintf(payload, "{"
                     "\"iat\":%lld,"
                     "\"exp\":%lld,"
                     "\"aud\":\"%s\","
                     "\"target_audience\":\"%s\","
                     "\"iss\":\"%s\","
                     "\"sub\":\"%s\""
                     "}", now, exp, JWT_GCP_TOKEN_URL, target_url, JWT_GCP_SERVICE_EMAIL, JWT_GCP_SERVICE_EMAIL);

//    ESP_LOGI(TAG, "JWT Raw Payload: %s, size: %d", payload, strlen(payload));

    char base64Payload[1000];
    base64url_encode(
            (unsigned char *) payload,  // Data to encode.
            strlen(payload),           // Length of data to encode.
            base64Payload);            // Base64 encoded data.
    uint8_t headerAndPayload[strlen(base64Header) + strlen(base64Payload)+2];
    sprintf((char *) headerAndPayload, "%s.%s", base64Header, base64Payload);

    mbedtls_pk_context pk_context;
    mbedtls_pk_init(&pk_context);
    size_t privateKeySize = private_pem_end - private_pem_start;
    // ESP_LOGD(TAG, "PK Size: %d", privateKeySize);

    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_ctr_drbg_init(&ctr_drbg);
    mbedtls_entropy_init(&entropy);

    const char *pers = "MyEntropy";

    mbedtls_ctr_drbg_seed(
            &ctr_drbg,
            mbedtls_entropy_func,
            &entropy,
            (const unsigned char *) pers,
            strlen(pers));

    int rc = mbedtls_pk_parse_key(&pk_context, private_pem_start, privateKeySize, NULL, 0, mbedtls_ctr_drbg_random,
                                  &ctr_drbg);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to mbedtls_pk_parse_key: %d (-0x%x): %s\n", rc, -rc, mbedtlsError(rc));
        return NULL;
    }

    uint8_t oBuf[5000];
    // ESP_LOGD(TAG, "1");
    // ESP_LOGD(TAG, "2");
    uint8_t digest[32];
    rc = mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), headerAndPayload, strlen((char *) headerAndPayload),
                    digest);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to mbedtls_md: %d (-0x%x): %s\n", rc, -rc, mbedtlsError(rc));
        return NULL;
    }
    // ESP_LOGD(TAG, "3");
    size_t retSize;
    rc = mbedtls_pk_sign(&pk_context, MBEDTLS_MD_SHA256, digest, sizeof(digest), oBuf, sizeof(oBuf), &retSize,
                         mbedtls_ctr_drbg_random, &ctr_drbg);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to mbedtls_pk_sign: %d (-0x%x): %s\n", rc, -rc, mbedtlsError(rc));
        return NULL;
    }

    // ESP_LOGD(TAG, "PK Sign size: %d", retSize);
    static char base64Signature[600];
    base64url_encode((unsigned char *) oBuf, retSize, base64Signature);

    static char signed_payload[1601];
    sprintf(signed_payload, "%s.%s", headerAndPayload, base64Signature);
    // ESP_LOGI(TAG, "JWT Encoded Payload: %s, size: %d", signed_payload, strlen(signed_payload));

    mbedtls_pk_free(&pk_context);

    return signed_payload;
}


