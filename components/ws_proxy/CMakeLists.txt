set(srcs "")
if(CONFIG_NBP_WS_PROXY)
    list(APPEND srcs "ws_proxy.cpp")
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
        esp_http_server
        lwip
        log
        freertos
)
