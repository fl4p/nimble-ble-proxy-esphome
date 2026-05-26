set(srcs "")
if(CONFIG_NBP_CLONE)
    list(APPEND srcs
        "clone.cpp"
        "clone_config.cpp"
        "clone_upstream.cpp"
        "clone_mirror.cpp"
    )
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS
        "."
        "../../include"
    REQUIRES
        ble_backend
        ble_httpd
        api_proto
        bt
        log
        nvs_flash
        esp_http_server
        esp_event
        freertos
)

# Pull NimBLE-CPP headers in via the same dep ble_backend uses.
# Only do this when the component has SRCS — with an empty source list
# IDF makes COMPONENT_LIB an INTERFACE library, which rejects PUBLIC.
if(CONFIG_NBP_CLONE)
    idf_component_get_property(nimble_lib h2zero__esp-nimble-cpp COMPONENT_LIB)
    if(nimble_lib)
        target_link_libraries(${COMPONENT_LIB} PUBLIC ${nimble_lib})
    endif()
endif()
