#pragma once

/* Copy to credentials.h (never committed) and fill in.
 * Secrets may be obfuscated so they are not readable at a glance (NOT encryption):
 * python3 <home-idf>/tools/obfuscate.py  ->  "obf1:..."  (plain values work too) */

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

/* hc-data Mosquitto password for user "water-controller" (see server/). Empty disables telemetry. */
#define MQTT_PASS                   ""
