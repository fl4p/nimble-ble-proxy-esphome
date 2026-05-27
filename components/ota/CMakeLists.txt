# app_update brings in esp_ota_ops headers used by ota.cpp. We keep it
# in PRIV_REQUIRES unconditionally — `if(CONFIG_NBP_OTA) APPEND` does
# not get picked up reliably by IDF's component dependency resolver
# in this position (the per-component CMakeLists evaluates before some
# CONFIG_* visibility passes). The actual /update handler code is
# #if-gated inside ota.cpp, so with NBP_OTA=n the OTA flash code paths
# are unreachable and the linker GCs them — same end-state binary, just
# without the dep-tree trim.
idf_component_register(
    SRCS "ota.cpp"
    INCLUDE_DIRS "." "../../include"
    REQUIRES esp_http_server
    PRIV_REQUIRES log app_update
)
