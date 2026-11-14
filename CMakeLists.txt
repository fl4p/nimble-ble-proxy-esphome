# Root CMakeLists for nimble-ble-proxy — ESP-IDF project.
cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS components)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# QEMU OOM self-test only (idf.py -B build-qemu-oom -DNBP_OOM_SELFTEST=1 …):
# app_main runs api_server::oom_selftest instead of the firmware. Defined for
# every component (main, api_server, ble_backend's gatt_discovery seam).
# Never in a firmware build.
if(NBP_OOM_SELFTEST)
    idf_build_set_property(COMPILE_DEFINITIONS "NBP_OOM_SELFTEST=1" APPEND)
endif()
# QEMU heap self-test only (idf.py -B build-qemu-heap -DNBP_HEAP_SELFTEST=1 …):
# app_main runs api_server::heap_selftest (send-queue cap, client refusal,
# stats boot race). Also arms a test-only lwIP input hook (nbp_lwip_hooks.h)
# that fakes a stalled peer. Never in a firmware build.
if(NBP_HEAP_SELFTEST)
    idf_build_set_property(COMPILE_DEFINITIONS "NBP_HEAP_SELFTEST=1" APPEND)
endif()

# QEMU static-lease self-test only (idf.py -B build-qemu-dhcps
# -DIDF_TARGET=esp32s3 -DNBP_DHCPS_SELFTEST=1 build, then … qemu): app_main
# runs dhcps_static_selftest_run() against IDF's DHCP server on lwIP
# loopback. Needs CONFIG_NBP_NAT_ROUTER (the hook wiring). Never in a
# firmware build.
if(NBP_DHCPS_SELFTEST)
    idf_build_set_property(COMPILE_DEFINITIONS "NBP_DHCPS_SELFTEST=1" APPEND)
endif()

# QEMU WebSocket-Origin / POST-confirm self-test only (idf.py -B
# build-qemu-ws -DIDF_TARGET=esp32s3 -DNBP_WS_SELFTEST=1 build, then … qemu):
# app_main runs ws_selftest::run() against the real httpd, api_server and
# ws_proxy on lwIP loopback. Never in a firmware build.
if(NBP_WS_SELFTEST)
    idf_build_set_property(COMPILE_DEFINITIONS "NBP_WS_SELFTEST=1" APPEND)
endif()

project(nimble_ble_proxy
    LANGUAGES C CXX
    VERSION 0.2.4)

# QEMU heap self-test on the C3: QEMU models no ADC, so IDF's boot-time ADC
# self-calibration (a constructor, pulled in with the PHY) spins forever in
# read_cal_channel before app_main. The self-test stubs it out
# (heap_selftest.cpp). Never in a firmware build.
if(NBP_HEAP_SELFTEST AND IDF_TARGET STREQUAL "esp32c3")
    target_link_options(${CMAKE_PROJECT_NAME}.elf PRIVATE
        "-Wl,--wrap=adc_calc_hw_calibration_code")
endif()

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
    # NAPT table size. lwip_napt.h only defaults IP_NAPT_MAX (512) under
    # #ifndef, and the sole consumer is lwIP's own ip4_napt.c, so a define on
    # the lwip target is enough. 512 x 32 B = 16 KB in one calloc, which the
    # ESP32-C3 can't satisfy and lwIP asserts on (issue #2). nat_router's
    # pre-enable heap check reads the same CONFIG value.
    target_compile_definitions(${lwip_lib} PRIVATE
        IP_NAPT_MAX=${CONFIG_NBP_NAT_TABLE_SIZE})
elseif(NBP_HEAP_SELFTEST)
    # The heap self-test's input hook (stalled peer, segment counter) lives
    # in the same header; a build without the NAT router (the C3 profile)
    # needs it wired too. Self-test builds only.
    idf_component_get_property(lwip_lib lwip COMPONENT_LIB)
    target_include_directories(${lwip_lib} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/components/dhcps_hostname/include")
    target_compile_definitions(${lwip_lib} PRIVATE
        ESP_IDF_LWIP_HOOK_FILENAME="nbp_lwip_hooks.h")
endif()

# IDF 6 builds with -Werror=missing-field-initializers in C++. These
# components register httpd routes with
# `httpd_uri_t x = {.uri=…, .method=…, .handler=…, .user_ctx=…}`, which
# omits the websocket members that CONFIG_HTTPD_WS_SUPPORT (selected by
# NBP_WS_PROXY) adds to the struct. Aggregate initialization zero-fills
# them — exactly the intended value (not a websocket route) — so the
# warning carries no information there. Scoped to those components only;
# IDF 5.5 is unaffected.
if(IDF_VERSION_MAJOR GREATER_EQUAL 6)
    foreach(comp api_server ble_clone nat_router ota wifi_sta)
        idf_component_get_property(comp_lib ${comp} COMPONENT_LIB)
        if(comp_lib)
            get_target_property(comp_type ${comp_lib} TYPE)
            if(NOT comp_type STREQUAL "INTERFACE_LIBRARY")
                target_compile_options(${comp_lib} PRIVATE
                    $<$<COMPILE_LANGUAGE:CXX>:-Wno-missing-field-initializers>)
            endif()
        endif()
    endforeach()
endif()
