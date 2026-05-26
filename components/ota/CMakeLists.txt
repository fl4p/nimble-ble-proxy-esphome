idf_component_register(
    SRCS "ota.cpp"
    INCLUDE_DIRS "." "../../include"
    REQUIRES app_update esp_http_server log
)
