# Root CMakeLists for nimble-ble-proxy — ESP-IDF project.
cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS components)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(nimble_ble_proxy
    LANGUAGES C CXX
    VERSION 0.1.0)
