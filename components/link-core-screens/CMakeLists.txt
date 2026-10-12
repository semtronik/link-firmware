idf_component_register(
    SRCS
        "src/screens_text.c"
        "src/ui_screen_about.c"
        "src/ui_screen_battery.c"
        "src/ui_screen_datetime.c"
        "src/ui_screen_factory_reset.c"
        "src/ui_screen_language.c"
        "src/ui_screen_shutdown.c"
        "src/ui_screen_splash.c"
        "src/ui_screen_sysinfo.c"
        "src/ui_screen_system.c"
        "src/ui_screen_time_edit.c"
        "src/ui_screen_time_onboarding.c"
    INCLUDE_DIRS "include"
    REQUIRES app_update esp_timer lvgl
             link-core-app link-core-board link-core-power link-core-ui
)
