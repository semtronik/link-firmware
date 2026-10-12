idf_component_register(
    SRCS "src/sx1262.c" "src/sx1262_radio.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_gpio esp_driver_spi esp_timer link-core-board
)
