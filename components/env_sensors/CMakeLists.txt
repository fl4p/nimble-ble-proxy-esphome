set(srcs "")
set(reqs "")
if(CONFIG_NBP_ENV_SENSORS)
    list(APPEND srcs "env_sensors.cpp")
    list(APPEND reqs espressif__onewire_bus)
endif()

# Gate off: INTERFACE library, main REQUIREs it unconditionally.
idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS
        "."
        "../../include"
    REQUIRES
        esp_driver_i2c
        esp_driver_gpio
        esp_http_server
        esp_timer
        nvs_flash
        log
        freertos
        ${reqs}
)
