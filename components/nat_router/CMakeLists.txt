set(srcs "")
if(CONFIG_NBP_NAT_ROUTER)
    list(APPEND srcs "nat_router.cpp")
endif()

# With the gate off, srcs is empty and IDF builds this as an INTERFACE
# library — main can REQUIRE it unconditionally and the #ifdef-guarded
# header compiles to nothing.
idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS
        "."
        "../../include"
    REQUIRES
        esp_wifi
        esp_coex
        esp_netif
        esp_event
        esp_http_server
        lwip
        nvs_flash
        log
        dhcps_hostname
)
