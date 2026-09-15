idf_component_register(
    SRCS "main.c"
         "tier0.c"
         "tier1.c"
         "rules.c"
         "valve.c"
         "settings.c"
         "flow.c"
         "protect.c"
         "events.c"
         "telemetry.c"
         "api.c"
         "testhw.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_driver_gpio esp_driver_pcnt esp_driver_ledc esp_wifi esp_timer esp_app_format nvs_flash mqtt json esp_http_server
    EMBED_TXTFILES ${project_dir}/certs/ota_server_cert_15.pem ${project_dir}/web/manifest.webmanifest
    EMBED_FILES ${project_dir}/web/apple-touch-icon.png)

home_idf_embed_gzip(${COMPONENT_LIB} ${project_dir}/web/login.html ${project_dir}/web/app.html)

# Rollback rehearsal only: idf.py -B build-panic -DWATER_TEST_PANIC=1 build (never ship)
if(WATER_TEST_PANIC)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE WATER_TEST_PANIC=1)
endif()

# Spare-board hardware tests only: idf.py -B build-testhw -DWATER_TEST_PULSES=1 build (never ship)
if(WATER_TEST_PULSES)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE WATER_TEST_PULSES=1 WATER_TEST_TIER0_S=150)
    if(WATER_TEST_SALT_HEX AND WATER_TEST_HASH_HEX)
        target_compile_definitions(${COMPONENT_LIB} PRIVATE WATER_TEST_SALT_HEX="${WATER_TEST_SALT_HEX}"
                                                            WATER_TEST_HASH_HEX="${WATER_TEST_HASH_HEX}")
    endif()
endif()

# Spare-board rehearsal: -DWATER_OTA_FILE=water-controller-spare.bin (production never downloads it)
if(WATER_OTA_FILE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE OTA_FILE="${WATER_OTA_FILE}")
endif()
