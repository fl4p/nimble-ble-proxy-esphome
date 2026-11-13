idf_component_register(
    SRCS "main.cpp" "ws_selftest.cpp"
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
        dhcps_hostname
        liveness_wdt
        serial_provision
        crash_capture
        esp_app_format
        oom_guard
        bench
)
