idf_component_register(
    SRCS "crash_capture.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server
    PRIV_REQUIRES
        esp_app_format
        esp_partition
        esp_system
        esp_timer
        espcoredump
        log
        spi_flash
)

# Route IDF's port-level panic handler through __wrap_esp_panic_handler
# (crash_capture.cpp) so the panic is copied to RTC memory before IDF
# prints it and reboots. panic_handler.c calls esp_panic_handler across
# object files, which is what --wrap rewrites.
if(CONFIG_NBP_CRASH_CAPTURE)
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_panic_handler")
endif()
