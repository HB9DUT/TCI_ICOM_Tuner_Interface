cmake_minimum_required(VERSION 3.16)

# Version aus dem Git-Tag (v2.4.0 -> 2.4.0); zwischen Releases z.B. 2.4.0-3-gabc1234-dirty
execute_process(
    COMMAND git describe --tags --always --dirty
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
    OUTPUT_VARIABLE PROJECT_VER
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
string(REGEX REPLACE "^v" "" PROJECT_VER "${PROJECT_VER}")
if(NOT PROJECT_VER)
    set(PROJECT_VER "0.0.0-dev")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(tci_icom_tuner_interface)
