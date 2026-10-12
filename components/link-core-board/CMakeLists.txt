idf_component_register(
    SRCS "src/board_core_bus.c" "src/board_i2c.c" "src/board_rtc.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_gpio esp_driver_i2c esp_rom
)
