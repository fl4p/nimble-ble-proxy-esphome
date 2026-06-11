set(srcs "")
if(CONFIG_NBP_LIVENESS_WDT)
    list(APPEND srcs "liveness_wdt.cpp")
endif()

# With the gate off, srcs is empty and IDF builds this as an INTERFACE
# library — main/api_server can REQUIRE it unconditionally and the
# #ifdef-guarded header compiles to nothing.
idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS
        "."
        "../../include"
    REQUIRES
        wifi_sta
        esp_netif
        esp_wifi
        lwip
        nvs_flash
        esp_timer
        esp_system
        freertos
        log
)
