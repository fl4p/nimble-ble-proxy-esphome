idf_component_register(
    SRCS "crash_capture.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server http_guard
    PRIV_REQUIRES
        esp_app_format
        esp_hw_support
        esp_partition
        esp_system
        esp_timer
        espcoredump
        log
        spi_flash
)

# QEMU self-test only (idf.py -DNBP_CRASH_SELFTEST=1 …): fault injection in
# the capture path plus a one-step-per-boot test driver. Never in firmware.
if(NBP_CRASH_SELFTEST)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE NBP_CRASH_SELFTEST=1)
endif()

# Route IDF's port-level panic handler through __wrap_esp_panic_handler
# (crash_capture.cpp) so the panic is copied to RTC memory before IDF
# prints it and reboots. panic_handler.c calls esp_panic_handler across
# object files, which is what --wrap rewrites.
if(CONFIG_NBP_CRASH_CAPTURE)
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_panic_handler")

    # Build guard: --wrap only rewrites references between object files, so
    # if IDF ever calls esp_panic_handler from its own object (or LTO merges
    # them) the wrapper is silently dead code. check_panic_wrap.py reads the
    # map's cross-reference table (IDF links with --cref) and fails the build
    # unless panic_handler.c.obj references __wrap_esp_panic_handler and
    # nothing but crash_capture calls the real one. The ELF target is created
    # after the components are processed, so attach at the end of the
    # top-level directory.
    # Variables in DEFER arguments are expanded when the call runs, in the
    # top-level scope, so hand the values over as global properties.
    idf_build_get_property(_nbp_python PYTHON)
    set_property(GLOBAL PROPERTY NBP_WRAP_CHECK_PYTHON "${_nbp_python}")
    set_property(GLOBAL PROPERTY NBP_WRAP_CHECK_SCRIPT
                 "${CMAKE_CURRENT_LIST_DIR}/check_panic_wrap.py")
    function(nbp_crash_capture_wrap_check)
        get_property(python GLOBAL PROPERTY NBP_WRAP_CHECK_PYTHON)
        get_property(script GLOBAL PROPERTY NBP_WRAP_CHECK_SCRIPT)
        set(elf "${CMAKE_PROJECT_NAME}.elf")
        if(NOT TARGET ${elf})
            message(FATAL_ERROR "crash_capture: ELF target ${elf} not found for the wrap check")
        endif()
        add_custom_command(TARGET ${elf} POST_BUILD
            COMMAND ${python} ${script}
                    "${CMAKE_BINARY_DIR}/${CMAKE_PROJECT_NAME}.map"
                    "${CMAKE_BINARY_DIR}/${elf}"
            COMMENT "crash_capture: checking the esp_panic_handler wrap"
            VERBATIM)
    endfunction()
    cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL nbp_crash_capture_wrap_check)
endif()
