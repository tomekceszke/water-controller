# One-shot OTA image that moves the legacy water-controller (ESP-IDF 5.1.1 bootloader, partitions_two_ota)
# onto the ESP-IDF 5.4.2 bootloader and partition table, then installs firmware 3.x. See docs/IDF5_MIGRATION.md.
# Build with tools/build_release.sh (it provides blobs/ from the firmware build).
cmake_minimum_required(VERSION 3.16)
set(PROJECT_VER "1.0.0")
if(DEFINED ENV{HOME_IDF_LOCAL})
    set(EXTRA_COMPONENT_DIRS $ENV{HOME_IDF_LOCAL})
endif()
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
if(DEFINED ENV{HOME_IDF_LOCAL})
    idf_build_set_property(DEPENDENCIES_LOCK ${CMAKE_CURRENT_LIST_DIR}/dependencies.local.lock)
endif()
project(water-migrator)
