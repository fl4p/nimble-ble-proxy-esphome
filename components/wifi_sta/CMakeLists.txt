set(priv_req log)
# AP fallback links in the captive httpd that POSTs creds back to NVS
# and reboots. esp_http_server is otherwise unused by wifi_sta and
# never appears in wifi_sta.h — PRIV_REQUIRES.
if(CONFIG_NBP_AP_FALLBACK)
    list(APPEND priv_req esp_http_server)
endif()
idf_component_register(
    SRCS "wifi_sta.cpp"
    INCLUDE_DIRS "." "../../include"
    REQUIRES esp_wifi esp_event esp_netif nvs_flash
    PRIV_REQUIRES ${priv_req}
)
