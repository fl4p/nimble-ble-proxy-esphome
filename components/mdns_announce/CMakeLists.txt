idf_component_register(
    SRCS "mdns_announce.cpp"
    INCLUDE_DIRS "." "../../include"
    REQUIRES mdns log esp_netif esp_event
)
