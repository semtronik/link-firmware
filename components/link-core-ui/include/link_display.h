/**
 * @file link_display.h
 * @brief ST7789 panel bring-up for the link base board.
 *
 * Step 1 of the display bring-up: SPI bus, panel init, solid fills and
 * on/off backlight.
 *
 * Expected boot sequence:
 *   link_display_panel_init();
 *   link_display_fill(LINK_DISPLAY_RED);
 *   link_display_backlight_on();
 *
 * Once link_lvgl_init() has run, LVGL owns the panel and the fill path
 * is closed - see link_display_attach_lvgl().
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_lcd_types.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Panel geometry, landscape. */
#define LINK_DISPLAY_H_RES 320
#define LINK_DISPLAY_V_RES 240

/** Test colors, RGB888. */
#define LINK_DISPLAY_BLACK 0x000000
#define LINK_DISPLAY_WHITE 0xFFFFFF
#define LINK_DISPLAY_RED   0xFF0000
#define LINK_DISPLAY_GREEN 0x00FF00
#define LINK_DISPLAY_BLUE  0x0000FF

/**
 * @brief Bring up the SPI bus and the ST7789 panel.
 *
 * On return the panel is initialised and display-on, but the backlight
 * is still off, so nothing is visible yet. Fill the framebuffer first,
 * then switch the backlight on, to avoid showing uninitialised memory.
 */
esp_err_t link_display_panel_init(void);

/**
 * @brief Fill the whole panel with a solid color.
 *
 * @param rgb888 Color as 0xRRGGBB.
 * @return ESP_ERR_INVALID_STATE once LVGL owns the panel: the transfer
 *         handshake belongs to LVGL then, and waiting on it here would
 *         block forever.
 */
esp_err_t link_display_fill(uint32_t rgb888);

/** @brief Switch the backlight on at full brightness. */
esp_err_t link_display_backlight_on(void);

/** @brief Switch the backlight off. */
esp_err_t link_display_backlight_off(void);

/**
 * @brief Set the backlight brightness.
 *
 * @param percent 0 (off) to 100 (full). Values above 100 are clamped.
 *
 * The duty is applied linearly: perceived brightness is not, so the low
 * end of the range looks coarser than the top.
 */
esp_err_t link_display_backlight_set(uint8_t percent);

/** @brief Last brightness applied, 0-100. */
uint8_t link_display_backlight_get(void);

/**
 * @brief Underlying panel handle, or NULL before init.
 *
 * Exposed so the LVGL layer can attach to it later.
 */
esp_lcd_panel_handle_t link_display_panel(void);

/**
 * @brief Hand the panel over to LVGL.
 *
 * Only one callback can be registered on the panel IO, and it is set at
 * init, long before LVGL exists. That one callback routes on this
 * handle: until it is set, a completed transfer releases the semaphore
 * link_display_fill() waits on; afterwards it calls
 * lv_display_flush_ready() instead.
 *
 * Call only once the display is created and its flush callback set -
 * the very next transfer will reach it. Closes link_display_fill() for
 * good.
 *
 * @param disp Display created by lv_display_create().
 */
void link_display_attach_lvgl(lv_display_t *disp);

/** @brief True once link_display_attach_lvgl() has run. */
bool link_display_lvgl_attached(void);

#ifdef __cplusplus
}
#endif
