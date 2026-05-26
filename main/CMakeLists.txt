idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "." "../include"
    REQUIRES wifi_sta mdns_announce ota api_server ble_backend
)
