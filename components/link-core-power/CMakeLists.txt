idf_component_register(
    SRCS "src/link_battery.c" "src/link_power.c" "src/power_key.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_timer link-core-board link-core-app
    PRIV_REQUIRES esp_driver_gpio
)
