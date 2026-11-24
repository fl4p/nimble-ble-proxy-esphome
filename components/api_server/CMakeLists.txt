set(embed_files "")
if(CONFIG_NBP_WIFI)
    # Dashboard HTML, served by stats.cpp::root_get. The BLE-only build
    # never reaches root_get, so don't burn the flash for unused bytes
    # when CONFIG_NBP_WIFI=n. index.html is embedded gzipped (below).
    set(embed_files "../../web/favicon.svg")
endif()

idf_component_register(
    SRCS
        "frame_codec.cpp"
        "api_server.cpp"
        "handshake.cpp"
        "bt_handlers.cpp"
        "stats.cpp"
        "oom_selftest.cpp"
        "heap_selftest.cpp"
    INCLUDE_DIRS
        "."
        "../../include"
    EMBED_FILES
        ${embed_files}
    REQUIRES
        api_proto
        ble_backend
        bt
        efuse
        esp_app_format
        esp_driver_tsens
        esp_event
        esp_http_server
        http_guard
        esp_netif
        esp_pm
        esp_wifi
        log
        lwip
        nvs_flash
        liveness_wdt
        oom_guard
)

# The dashboard is stored gzip-compressed (120 KB -> ~38 KB of flash) and
# served with Content-Encoding: gzip (stats.cpp root_get). mtime=0 keeps
# the bytes, and so the image, reproducible.
if(CONFIG_NBP_WIFI)
    idf_build_get_property(python PYTHON)
    set(_html "${CMAKE_CURRENT_SOURCE_DIR}/../../web/index.html")
    set(_html_gz "${CMAKE_CURRENT_BINARY_DIR}/index.html.gz")
    add_custom_command(OUTPUT "${_html_gz}"
        COMMAND "${python}" -c
            "import gzip,sys; d=open(sys.argv[1],'rb').read(); open(sys.argv[2],'wb').write(gzip.compress(d,9,mtime=0))"
            "${_html}" "${_html_gz}"
        DEPENDS "${_html}"
        VERBATIM)
    target_add_binary_data(${COMPONENT_LIB} "${_html_gz}" BINARY
        RENAME_TO index_html_gz)
endif()

# Patch the managed esp-nimble-cpp header to add `setNotifyCallback`.
# bt_handlers needs to register a notify callback WITHOUT writing the
# CCCD — bleak-esphome writes the descriptor separately a moment later;
# emitting it twice trips some BMS peripherals (e.g. ANT-BLE20PHUB,
# see FINDINGS.md). Upstream `subscribe()` always writes the CCCD, so
# we add a one-line public accessor.
#
# This patch USED to live directly in managed_components/ as a manual
# edit, but the IDF component manager wipes it on re-fetch (and the
# registry has been observed to roll the same 2.5.0 component_hash to
# different file contents — discovered 2026-05-27 when integrating into
# the esp32_nat_router_extended project). Applying at CMake configure
# time keeps the build self-healing. Idempotent — first build patches,
# subsequent builds no-op.
set(_nimble_hdr
    "${CMAKE_SOURCE_DIR}/managed_components/h2zero__esp-nimble-cpp/src/NimBLERemoteCharacteristic.h")
if(EXISTS "${_nimble_hdr}")
    file(READ "${_nimble_hdr}" _nimble_src)
    string(FIND "${_nimble_src}" "setNotifyCallback" _has_patch)
    if(_has_patch EQUAL -1)
        set(_anchor "bool unsubscribe(bool response = true) const;")
        # Use a single CMake string literal (not multiple quoted args —
        # set() would join them with ';' and string(REPLACE) would emit
        # those semicolons into the file, producing ugly-but-legal C++).
        set(_patch "${_anchor}
    // Register/clear the notify callback WITHOUT issuing a CCCD
    // write. bleak-esphome writes the CCCD separately; emitting
    // it twice trips some BMS peripherals (see FINDINGS.md).
    void setNotifyCallback(const notify_callback cb) const { m_notifyCallback = cb; }")
        string(REPLACE "${_anchor}" "${_patch}" _nimble_src "${_nimble_src}")
        file(WRITE "${_nimble_hdr}" "${_nimble_src}")
        message(STATUS "patched setNotifyCallback into ${_nimble_hdr}")
    endif()
endif()

# Link NimBLE-CPP for bt_handlers (which #includes NimBLEDevice.h under
# CONFIG_NBP_BLE). The esp-nimble-cpp dependency is optional (gated on
# $CONFIG{NBP_BLE}) and the component manager doesn't auto-inject optional
# deps into REQUIRES, so wire it explicitly — same pattern as ble_backend /
# ble_clone / ble_httpd. A BLE-free build omits it (bt_handlers is a stub).
if(CONFIG_NBP_BLE)
    idf_component_get_property(nimble_lib h2zero__esp-nimble-cpp COMPONENT_LIB)
    if(nimble_lib)
        target_link_libraries(${COMPONENT_LIB} PUBLIC ${nimble_lib})
    endif()
endif()
