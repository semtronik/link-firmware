idf_component_register(
    SRCS
        "src/link_events.c"
        "src/link_factory.c"
        "src/link_i18n.c"
        "src/link_ota.c"
        "src/link_prefs.c"
        "src/link_proto.c"
        "src/link_serial.c"
        "src/link_time.c"
    INCLUDE_DIRS "include"
    REQUIRES app_update esp_timer fatfs mbedtls nvs_flash
             wear_levelling
             link-core-board
    PRIV_REQUIRES esp_driver_usb_serial_jtag
)
