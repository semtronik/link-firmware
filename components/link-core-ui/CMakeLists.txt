idf_component_register(
    SRCS
        "src/fonts/link_font_12.c"
        "src/fonts/link_font_14.c"
        "src/fonts/link_font_16.c"
        "src/fonts/link_font_28.c"
        "src/fonts/link_icons_20.c"
        "src/link_display.c"
        "src/link_lvgl.c"
        "src/link_lvgl_mem.c"
        "src/link_screenshot.c"
        "src/link_theme.c"
        "src/link_touch.c"
        "src/st7789t/st7789t_panel.c"
        "src/ui_helpers.c"
        "src/ui_touch.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    # esp_lcd_touch is public, not private: link_touch.h exposes its
    # handle type to whoever includes it. lvgl likewise: link_theme.h
    # and link_lvgl.h are built on its types.
    REQUIRES esp_lcd esp_lcd_touch esp_lcd_touch_ft5x06 esp_timer lvgl
             link-core-app link-core-board link-core-power
    PRIV_REQUIRES esp_driver_gpio esp_driver_i2c esp_driver_ledc esp_driver_spi
                  mbedtls
)

# Only LVGL calls the allocator, and it links after us.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u lv_mem_init")
