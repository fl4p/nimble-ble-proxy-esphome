idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "." "../include"
    REQUIRES
        ble_backend
        wifi_sta
        mdns_announce
        ota
        api_server
        ble_clone
        ble_httpd
        ws_proxy
        nat_router
        liveness_wdt
)
