# The whole backend is NimBLE-dependent, so it compiles only when the
# NBP_BLE master gate is on. With NBP_BLE=n the NimBLE stack itself is
# absent (BT_ENABLED unset) and every caller (main, api_server) guards
# its ble_backend:: calls behind CONFIG_NBP_BLE — so an empty source list
# (→ INTERFACE component) is exactly right. Mirrors ble_clone / ble_httpd.
set(srcs "")
if(CONFIG_NBP_BLE)
    list(APPEND srcs
        "address.cpp"
        "publish.cpp"
        "scanner.cpp"
        "connection.cpp"
        "gatt_discovery.cpp"
        "ble_backend.cpp"
        "bthome.cpp"
    )
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS
        "."
        "../../include"
    REQUIRES
        api_proto
        bt
        log
        esp_event
        esp_timer
        freertos
)

# Pull in NimBLE-CPP headers/lib. The esp-nimble-cpp dependency in
# idf_component.yml is *optional* (gated on $CONFIG{NBP_BLE}), and the
# component manager does NOT auto-inject an optional dependency into this
# component's REQUIRES — so link it explicitly, exactly as ble_clone /
# ble_httpd do. Guarded so an empty (INTERFACE) BLE-free build skips it.
if(CONFIG_NBP_BLE)
    idf_component_get_property(nimble_lib h2zero__esp-nimble-cpp COMPONENT_LIB)
    if(nimble_lib)
        target_link_libraries(${COMPONENT_LIB} PUBLIC ${nimble_lib})
    endif()
endif()
