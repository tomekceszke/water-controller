cmake_minimum_required(VERSION 3.16)
set(PROJECT_VER "3.0.0")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# build.sh with a local home-idf checkout writes its own lock, so the committed one stays pinned to git
if(DEFINED WC_DEPENDENCIES_LOCK)
    idf_build_set_property(DEPENDENCIES_LOCK ${WC_DEPENDENCIES_LOCK})
endif()
project(water-controller)
