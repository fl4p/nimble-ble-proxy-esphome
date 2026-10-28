# Root CMakeLists for nimble-ble-proxy — ESP-IDF project.
cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS components)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# IDF 6 builds with -Werror=missing-field-initializers in C++. Every
# `httpd_uri_t x = {.uri=…, .method=…, .handler=…, .user_ctx=…}` in the
# tree omits the websocket members that CONFIG_HTTPD_WS_SUPPORT (selected
# by NBP_WS_PROXY) adds to the struct. Aggregate initialization zero-fills
# them, which is exactly the intended value (not a websocket route), so
# the warning carries no information here. IDF 5.5 is unaffected.
if(IDF_VERSION_MAJOR GREATER_EQUAL 6)
    idf_build_set_property(CXX_COMPILE_OPTIONS "-Wno-missing-field-initializers" APPEND)
endif()

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
