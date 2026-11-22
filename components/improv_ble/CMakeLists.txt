set(srcs "")
if(CONFIG_NBP_IMPROV)
    list(APPEND srcs "improv_ble.cpp" "improv_proto.cpp")
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS
        "."
        "../../include"
    REQUIRES
        wifi_sta
        bt
        esp_app_format
        esp_timer
        log
        freertos
)

# NimBLE-CPP headers, as in ble_httpd. Only with SRCS: an empty source list
# makes COMPONENT_LIB an INTERFACE library, which rejects PUBLIC.
if(CONFIG_NBP_IMPROV)
    idf_component_get_property(nimble_lib h2zero__esp-nimble-cpp COMPONENT_LIB)
    if(nimble_lib)
        target_link_libraries(${COMPONENT_LIB} PUBLIC ${nimble_lib})
    endif()
endif()
