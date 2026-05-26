set(srcs "")
if(CONFIG_NBP_BLE_HTTPD)
    list(APPEND srcs "ble_httpd.cpp")
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS
        "."
        "../../include"
    REQUIRES
        api_server
        ble_backend
        ble_clone
        bt
        log
        freertos
)

# Pull NimBLE-CPP headers in via the same dep ble_backend uses.
# Only do this when the component has SRCS — with an empty source list
# IDF makes COMPONENT_LIB an INTERFACE library, which rejects PUBLIC.
if(CONFIG_NBP_BLE_HTTPD)
    idf_component_get_property(nimble_lib h2zero__esp-nimble-cpp COMPONENT_LIB)
    if(nimble_lib)
        target_link_libraries(${COMPONENT_LIB} PUBLIC ${nimble_lib})
    endif()
endif()
