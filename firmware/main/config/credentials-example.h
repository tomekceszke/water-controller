#pragma once

/* Copy to credentials.h (never committed) and fill in. */

#define WIFI_SSID                   ""
#define WIFI_PASS                   ""

/* Authorization header for the /admin endpoints (scripts). Empty locks them. */
#define HEADER_AUTHORIZATION_VALUE  ""

/* Web login: output of home-idf tools/hash_password.py. Empty disables the UI. */
#define AUTH_PASSWORD_ITERATIONS    20000
#define AUTH_PASSWORD_SALT_HEX      ""
#define AUTH_PASSWORD_HASH_HEX      ""

/* ntfy.sh topics (keep them unguessable). Empty disables. */
#define NTFY_TOPIC                  ""
#define NTFY_ERROR_TOPIC            ""

/* hc-data Mosquitto user (write water/+/..., see server/). Empty password disables telemetry. */
#define MQTT_USER                   "water-controller"
#define MQTT_PASS                   ""
