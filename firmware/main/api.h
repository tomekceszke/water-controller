#pragma once

#include "cJSON.h"

/* Registers the water-controller routes on the home-idf HTTP server. */
void api_start(void);

/* The full status document, as served by GET /api/status and GET /admin/status. Also the retained
 * hi_mqtt state topic, so both always describe the device the same way. The caller owns the result.
 * Safe to call from another task while a request is being served. */
cJSON *api_status_json(void);
