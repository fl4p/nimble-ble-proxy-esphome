# WHOLE_ARCHIVE: oom_guard.cpp replaces the global operator new family. As an
# ordinary archive member it would only be pulled in if the linker happened
# to scan this library before libstdc++ while _Znwj was still undefined; as a
# whole archive its definitions are in place before libstdc++ is searched,
# so libstdc++'s new_op*.o are never linked. check_new_override.py verifies
# that after every link.
idf_component_register(
    SRCS "oom_guard.cpp"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES
        esp_system
        esp_timer
        freertos
        heap
        log
    WHOLE_ARCHIVE
)

# Build guard (same mechanism as crash_capture's wrap check): fail the build
# and delete the ELF unless the map's cross-reference table shows every
# replaced allocation function defined by oom_guard.cpp.obj. A libstdc++
# definition winning would silently bring back abort-on-OOM in
# `new (std::nothrow)`.
idf_build_get_property(_nbp_python PYTHON)
set_property(GLOBAL PROPERTY NBP_NEW_CHECK_PYTHON "${_nbp_python}")
set_property(GLOBAL PROPERTY NBP_NEW_CHECK_SCRIPT
             "${CMAKE_CURRENT_LIST_DIR}/check_new_override.py")
function(nbp_oom_guard_new_check)
    get_property(python GLOBAL PROPERTY NBP_NEW_CHECK_PYTHON)
    get_property(script GLOBAL PROPERTY NBP_NEW_CHECK_SCRIPT)
    set(elf "${CMAKE_PROJECT_NAME}.elf")
    if(NOT TARGET ${elf})
        message(FATAL_ERROR "oom_guard: ELF target ${elf} not found for the new-override check")
    endif()
    add_custom_command(TARGET ${elf} POST_BUILD
        COMMAND ${python} ${script}
                "${CMAKE_BINARY_DIR}/${CMAKE_PROJECT_NAME}.map"
                "${CMAKE_BINARY_DIR}/${elf}"
        COMMENT "oom_guard: checking the operator new override"
        VERBATIM)
endfunction()
cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL nbp_oom_guard_new_check)
