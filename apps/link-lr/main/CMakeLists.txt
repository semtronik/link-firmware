idf_component_register(
    SRCS "ui_screen_setup.c" "ui_text.c" "link_proto_rec.c" "ui_view_stats.c" "link_profile.c" "ui_screen_capture.c" "link_proto_radio.c" "link_radio.c" "link_rec.c" "link_scan.c"
         "link_regul.c"
         "main.c"
         "ui_manager.c" "ui_param_edit.c"
         "ui_screen_config.c" "ui_screen_menu.c" "link_stats.c"
         "ui_screen_scan.c" "ui_sysinfo_radio.c"
    INCLUDE_DIRS "."
    REQUIRES app_update esp_timer fatfs mbedtls nvs_flash
             link-core-app link-core-board link-core-power link-core-screens
             link-core-ui link-mod-lr
)
