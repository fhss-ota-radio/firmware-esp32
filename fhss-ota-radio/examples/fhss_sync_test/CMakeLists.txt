cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS
    "../../components/fhss_core"
    "../../components/rf_transport"
    "../../components/fhss_service"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(fhss-sync-test)

