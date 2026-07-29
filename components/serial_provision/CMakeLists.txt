idf_component_register(
    SRCS "serial_provision.cpp"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash esp_timer esp_driver_uart esp_driver_usb_serial_jtag
    PRIV_REQUIRES log
)
