# app_update brings in esp_ota_ops headers used by ota.cpp. We keep it
# in PRIV_REQUIRES unconditionally — `if(CONFIG_NBP_OTA) APPEND` does
# not get picked up reliably by IDF's component dependency resolver
# in this position (the per-component CMakeLists evaluates before some
# CONFIG_* visibility passes). The actual /update handler code is
# #if-gated inside ota.cpp, so with NBP_OTA=n the OTA flash code paths
# are unreachable and the linker GCs them — same end-state binary, just
# without the dep-tree trim.
#
# ota_verify.cpp (app rollback: mark the running image valid or roll it
# back) is always built: an image must confirm itself whenever an app with
# rollback installed it, whatever this build's NBP_OTA/NBP_WIFI say.
idf_component_register(
    SRCS "ota.cpp" "ota_verify.cpp"
    INCLUDE_DIRS "." "../../include"
    REQUIRES esp_http_server esp_partition
    PRIV_REQUIRES log app_update bootloader_support nvs_flash esp_netif lwip esp_timer
)

# Known-bad calibration of the QEMU OTA-rollback self-test (NBP_OTA_SELFTEST
# is set for every component in the root CMakeLists): mark-valid claims
# success and writes nothing. Never in a firmware build.
if(NBP_OTA_SELFTEST_BREAK)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE NBP_OTA_SELFTEST_BREAK=1)
endif()
