set(embed_files "")
if(CONFIG_NBP_WIFI)
    # Dashboard HTML, served by stats.cpp::root_get. The BLE-only build
    # never reaches root_get, so don't burn ~20 KB of flash for unused
    # bytes when CONFIG_NBP_WIFI=n.
    set(embed_files "../../web/index.html")
endif()

idf_component_register(
    SRCS
        "frame_codec.cpp"
        "api_server.cpp"
        "handshake.cpp"
        "bt_handlers.cpp"
        "stats.cpp"
    INCLUDE_DIRS
        "."
        "../../include"
    EMBED_FILES
        ${embed_files}
    REQUIRES
        api_proto
        ble_backend
        bt
        esp_driver_tsens
        esp_event
        esp_http_server
        esp_netif
        esp_pm
        esp_wifi
        log
        lwip
        nvs_flash
)
