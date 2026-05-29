set(srcs "")
if(CONFIG_NBP_NAT_ROUTER)
    list(APPEND srcs "dhcps_hostname.c")
endif()

# Gate matches nat_router: with NAT off, srcs is empty and this builds as an
# INTERFACE library, the lwIP hook is not wired (see root CMakeLists), and
# nbp_dhcps_post_state is never referenced. The `include/` dir still carries
# nbp_lwip_hooks.h, which the root CMakeLists adds to lwIP's include path.
idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "include"
    REQUIRES lwip freertos
)
