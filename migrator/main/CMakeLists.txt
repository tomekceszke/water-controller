set(FW ${project_dir}/../firmware)
idf_component_register(SRCS "main.c"
    INCLUDE_DIRS ${FW}/main ${project_dir}/blobs
    PRIV_REQUIRES esp_driver_gpio nvs_flash
    EMBED_TXTFILES ${FW}/certs/ota_server_cert_15.pem
    # From the firmware build, copied by home-idf tools/build_migrator.sh together with blobs_manifest.h
    EMBED_FILES ${project_dir}/blobs/bootloader.bin ${project_dir}/blobs/partition-table.bin)

# Spare-board rehearsal: -DWATER_OTA_FILE=water-controller-spare.bin (production never downloads it)
if(WATER_OTA_FILE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE OTA_FILE="${WATER_OTA_FILE}")
endif()
