# Root CMakeLists for nimble-ble-proxy — ESP-IDF project.
cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS components)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(nimble_ble_proxy
    LANGUAGES C CXX
    VERSION 0.1.0)

# DHCP client-hostname capture (NAT/SoftAP only). IDF 5.5's DHCP server
# discards option 12, so we inject a custom lwIP hook header to read it —
# the sanctioned ESP_IDF_LWIP_HOOK_FILENAME mechanism (see lwipopts.h and
# lwip_default_hooks.h, which `#include` it). This compiles the hook macro
# into lwIP's dhcpserver.c; the implementation lives in the dhcps_hostname
# component, pulled into the link by nat_router's REQUIRES. Done here, after
# project(), because CONFIG_* and the lwip COMPONENT_LIB are only resolved
# once components are registered.
if(CONFIG_NBP_NAT_ROUTER)
    idf_component_get_property(lwip_lib lwip COMPONENT_LIB)
    target_include_directories(${lwip_lib} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/components/dhcps_hostname/include")
    target_compile_definitions(${lwip_lib} PRIVATE
        ESP_IDF_LWIP_HOOK_FILENAME="nbp_lwip_hooks.h")
endif()
