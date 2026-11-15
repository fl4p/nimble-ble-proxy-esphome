set(srcs "")
if(CONFIG_NBP_MODBUS_BRIDGE)
    list(APPEND srcs "modbus_bridge.cpp")
endif()

# Gate off: INTERFACE library, main REQUIREs it unconditionally.
idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS
        "."
        "../../include"
    REQUIRES
        esp_driver_uart
        esp_driver_gpio
        esp_http_server
        esp_netif
        lwip
        nvs_flash
        log
        freertos
)
