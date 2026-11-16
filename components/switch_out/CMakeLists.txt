set(srcs "")
if(CONFIG_NBP_SWITCH_OUT)
    list(APPEND srcs "switch_out.cpp")
endif()

# Gate off: INTERFACE library, main REQUIREs it unconditionally.
idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS
        "."
        "../../include"
    REQUIRES
        esp_driver_gpio
        esp_http_server
        esp_netif
        esp_timer
        lwip
        nvs_flash
        log
)
