set(srcs "")
if(CONFIG_NBP_BENCH)
    list(APPEND srcs "bench.cpp")
endif()

# With the gate off, srcs is empty and IDF builds this as an INTERFACE
# library, so main can REQUIRE it unconditionally.
idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "."
    REQUIRES lwip heap esp_timer freertos log
)
