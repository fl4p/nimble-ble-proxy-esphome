idf_component_register(
    SRCS "http_guard.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server
    PRIV_REQUIRES esp_system heap log lwip
)

# Every httpd_register_uri_handler() call in our components goes through
# __wrap_httpd_register_uri_handler (http_guard.cpp), which puts a
# trampoline in front of the handler: on failure it logs the URI and the
# heap, and makes httpd's close abortive. --wrap rewrites only references
# between object files; a call that escapes it just loses the logging.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=httpd_register_uri_handler")
