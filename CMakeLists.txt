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
    # Its test clients are lwIP sockets standing in for Linux clients; give
    # them a Linux-like receive queue (see sdkconfig.defaults.heaptest). Read
    # when the build dir's sdkconfig is created: use a fresh build dir.
    if(NOT SDKCONFIG_DEFAULTS)
        set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;sdkconfig.defaults.heaptest")
    endif()
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

# QEMU OTA-rollback self-test only (idf.py -B build-qemu-ota
# -DIDF_TARGET=esp32s3 -DNBP_OTA_SELFTEST=1 build, then … qemu): after
# ota_verify::early_init(), app_main runs ota_verify::selftest::run(), which
# installs copies of itself into the other slot and checks mark-valid, the
# rollback on a panic and on the deadline against the real bootloader.
# sdkconfig.defaults.otatest shortens the verify times. Never in a firmware
# build.
if(NBP_OTA_SELFTEST)
    idf_build_set_property(COMPILE_DEFINITIONS "NBP_OTA_SELFTEST=1" APPEND)
    if(NOT SDKCONFIG_DEFAULTS)
        set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;sdkconfig.defaults.otatest")
    endif()
endif()

# QEMU OTA-signature self-test only (idf.py -B build-qemu-sig
# -DIDF_TARGET=esp32s3 -DSDKCONFIG=build-qemu-sig/sdkconfig
# -DNBP_SIG_SELFTEST=1 build, then … qemu): app_main runs
# ota::sig_selftest::run() against the real /update handler on lwIP
# loopback. Needs the dev signing key. Never in a firmware build.
if(NBP_SIG_SELFTEST)
    idf_build_set_property(COMPILE_DEFINITIONS "NBP_SIG_SELFTEST=1" APPEND)
endif()

project(nimble_ble_proxy
    LANGUAGES C CXX
    VERSION 0.2.8)

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

# Release-variant identity, reported in GET /appinfo as "variant". The
# dashboard offers an update image only to a device whose variant names
# exactly one release asset (nimble_ble_proxy-<variant>-ota.bin), so a
# wrong label could put a stock image on a custom build (PSRAM, a preset
# switch pin, ...). CI sets it (-DNBP_VARIANT=<target>-ble-on|ble-off, see
# .github/workflows/build.yml); every other build is "custom" and gets no
# offer. A stock label is refused unless the build really is the stock
# configuration: the target and BLE gate match the name, the defaults list
# is the one CI uses, and none of the opt-in fragments' features are on.
if(NOT NBP_VARIANT)
    set(NBP_VARIANT "custom")
endif()
if(NOT NBP_VARIANT STREQUAL "custom")
    if(CONFIG_NBP_BLE)
        set(_nbp_stock_variant "${IDF_TARGET}-ble-on")
        set(_nbp_stock_defaults "sdkconfig.defaults")
    else()
        set(_nbp_stock_variant "${IDF_TARGET}-ble-off")
        set(_nbp_stock_defaults "sdkconfig.defaults;.github/build-no-ble.defaults")
    endif()
    # IDF hands SDKCONFIG_DEFAULTS back as absolute paths; compare relative.
    set(_nbp_defaults "")
    set(_nbp_raw "${SDKCONFIG_DEFAULTS}")
    if(NOT _nbp_raw)
        set(_nbp_raw "sdkconfig.defaults")
    endif()
    foreach(_nbp_f IN LISTS _nbp_raw)
        get_filename_component(_nbp_abs "${_nbp_f}" ABSOLUTE
                               BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
        file(RELATIVE_PATH _nbp_rel "${CMAKE_CURRENT_SOURCE_DIR}" "${_nbp_abs}")
        list(APPEND _nbp_defaults "${_nbp_rel}")
    endforeach()
    if(NOT NBP_VARIANT STREQUAL _nbp_stock_variant)
        message(FATAL_ERROR "NBP_VARIANT=${NBP_VARIANT} but this build is "
            "${_nbp_stock_variant} (IDF_TARGET + CONFIG_NBP_BLE)")
    endif()
    if(NOT _nbp_defaults STREQUAL _nbp_stock_defaults)
        message(FATAL_ERROR "NBP_VARIANT=${NBP_VARIANT} needs "
            "SDKCONFIG_DEFAULTS=\"${_nbp_stock_defaults}\" (got "
            "\"${_nbp_defaults}\"); label other builds custom")
    endif()
    foreach(_nbp_opt SPIRAM NBP_MODBUS_BRIDGE NBP_ENV_SENSORS
                     NBP_CRASH_TEST)
        if(CONFIG_${_nbp_opt})
            message(FATAL_ERROR "NBP_VARIANT=${NBP_VARIANT} with "
                "CONFIG_${_nbp_opt}=y is not a release build; label it custom")
        endif()
    endforeach()
    # The switch output is in every image; only a preset pin is custom
    # (a release image must touch no GPIO until one is set at runtime).
    if(DEFINED CONFIG_NBP_SWITCH_GPIO AND NOT CONFIG_NBP_SWITCH_GPIO EQUAL -1)
        message(FATAL_ERROR "NBP_VARIANT=${NBP_VARIANT} with "
            "CONFIG_NBP_SWITCH_GPIO=${CONFIG_NBP_SWITCH_GPIO} is not a release "
            "build (release images preset no pin); label it custom")
    endif()
    foreach(_nbp_st NBP_DHCPS_SELFTEST NBP_WS_SELFTEST NBP_HEAP_SELFTEST
                    NBP_OOM_SELFTEST NBP_CRASH_SELFTEST)
        if(${_nbp_st})
            message(FATAL_ERROR "NBP_VARIANT=${NBP_VARIANT} on a ${_nbp_st} build")
        endif()
    endforeach()
endif()
message(STATUS "nimble-ble-proxy release variant: ${NBP_VARIANT}")
idf_component_get_property(_nbp_api_lib api_server COMPONENT_LIB)
target_compile_definitions(${_nbp_api_lib} PRIVATE NBP_VARIANT="${NBP_VARIANT}")

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

# App image signing (CLAUDE.md "App image signing", docs/signing.md).
# sdkconfig.defaults turns on CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT but
# leaves CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES off, so IDF writes an
# unsigned nimble_ble_proxy.bin (what CI publishes; releases are signed by
# scripts/release_sign.py). Local builds sign it in place here with the dev
# key, so `idf.py flash` and the curl OTA keep working on dev-key devices.
# The unsigned image stays next to it as nimble_ble_proxy-unsigned.bin.
#   key: -DNBP_SIGNING_KEY=<pem> | env NBP_SIGNING_KEY |
#        ~/.config/nbp-signing/dev-key.pem; "none" = leave unsigned.
if(CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT AND
   NOT CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES)
    if(DEFINED NBP_SIGNING_KEY)
        set(nbp_sign_key "${NBP_SIGNING_KEY}")
    elseif(DEFINED ENV{NBP_SIGNING_KEY})
        set(nbp_sign_key "$ENV{NBP_SIGNING_KEY}")
    else()
        set(nbp_sign_key "$ENV{HOME}/.config/nbp-signing/dev-key.pem")
    endif()
    idf_build_get_property(build_dir BUILD_DIR)
    idf_build_get_property(python PYTHON)
    idf_build_get_property(project_bin PROJECT_BIN)
    set(nbp_sign_py "${CMAKE_CURRENT_SOURCE_DIR}/scripts/nbp_sign.py")
    if(nbp_sign_key STREQUAL "none" OR NOT EXISTS "${nbp_sign_key}")
        if(NBP_SIG_SELFTEST)
            message(FATAL_ERROR "NBP_SIG_SELFTEST needs a signing key "
                "(${nbp_sign_key}); see docs/signing.md")
        endif()
        message(WARNING "nbp: no signing key (${nbp_sign_key}): "
            "${project_bin} stays UNSIGNED. A device running a signed image "
            "refuses it over OTA, and a device flashed with it over USB "
            "refuses every OTA. See docs/signing.md.")
        add_custom_command(TARGET app POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E echo
                "nbp: ${project_bin} was not signed by this build (no key at ${nbp_sign_key}); check with scripts/nbp_sign.py info"
            VERBATIM)
    else()
        add_custom_command(OUTPUT "${build_dir}/.nbp_signed_stamp"
            COMMAND ${python} "${nbp_sign_py}" build-sign
                --bin "${build_dir}/${project_bin}" --key "${nbp_sign_key}"
                --stamp "${build_dir}/.nbp_signed_stamp"
            DEPENDS "${build_dir}/.bin_timestamp" "${nbp_sign_key}" "${nbp_sign_py}"
            VERBATIM
            COMMENT "Signing ${project_bin} with ${nbp_sign_key}")
        add_custom_target(nbp_sign_app DEPENDS "${build_dir}/.nbp_signed_stamp")
        add_dependencies(gen_project_binary nbp_sign_app)
        # IDF's own POST_BUILD line says "App built but not signed" (it means
        # its build-time signing, which is off); correct it right after.
        add_custom_command(TARGET app POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E echo
                "nbp: ignore the line above: ${project_bin} was signed by scripts/nbp_sign.py (key ${nbp_sign_key})"
            VERBATIM)

        if(NBP_SIG_SELFTEST)
            # Throwaway "attacker" key, generated into the build dir.
            set(nbp_wrong_key "${build_dir}/sigtest-wrong-key.pem")
            if(NOT EXISTS "${nbp_wrong_key}")
                execute_process(COMMAND ${python} -m espsecure generate-signing-key
                        --version 2 --scheme rsa3072 "${nbp_wrong_key}"
                    RESULT_VARIABLE rc OUTPUT_QUIET)
                if(NOT rc EQUAL 0)
                    message(FATAL_ERROR "could not generate ${nbp_wrong_key}")
                endif()
            endif()
            execute_process(COMMAND ${python} "${nbp_sign_py}" fingerprint "${nbp_sign_key}"
                OUTPUT_VARIABLE nbp_key_fp OUTPUT_STRIP_TRAILING_WHITESPACE
                RESULT_VARIABLE rc)
            if(NOT rc EQUAL 0)
                message(FATAL_ERROR "fingerprint of ${nbp_sign_key} failed")
            endif()
            idf_component_get_property(ota_lib ota COMPONENT_LIB)
            target_compile_definitions(${ota_lib} PRIVATE
                NBP_SIG_SELFTEST_KEY="${nbp_key_fp}")
            set(nbp_blob "${build_dir}/sigtest-blob.bin")
            add_custom_command(OUTPUT "${nbp_blob}"
                COMMAND ${python} "${nbp_sign_py}" selftest-blob
                    --bin "${build_dir}/${project_bin}" --wrong-key "${nbp_wrong_key}"
                    -o "${nbp_blob}"
                DEPENDS "${build_dir}/.nbp_signed_stamp" "${nbp_sign_py}"
                VERBATIM)
            add_custom_target(nbp_sigtest_blob ALL DEPENDS "${nbp_blob}")
            # Last 8 KiB of the coredump partition (0x3E0000 + 0x20000); the
            # core dump check at boot only reads the start of the partition.
            esptool_py_flash_target_image(flash sigtest_blob 0x3FE000 "${nbp_blob}")
        endif()
    endif()
endif()
