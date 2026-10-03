cmake_minimum_required(VERSION 3.16)

# Version aus dem Git-Tag (v2.4.0 -> 2.4.0); zwischen Releases z.B. 2.4.0-3-gabc1234-dirty.
# Bei PlatformIO-Builds ermittelt version.py die Version und übergibt sie in FW_VERSION.
if(DEFINED ENV{FW_VERSION})
    set(PROJECT_VER "$ENV{FW_VERSION}")
else()
    execute_process(
        COMMAND git describe --tags --always --dirty
        WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
        OUTPUT_VARIABLE PROJECT_VER
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
endif()
string(REGEX REPLACE "^v" "" PROJECT_VER "${PROJECT_VER}")
if(NOT PROJECT_VER)
    set(PROJECT_VER "0.0.0-dev")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(tci_icom_tuner_interface)
