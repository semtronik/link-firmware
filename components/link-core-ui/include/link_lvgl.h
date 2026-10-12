/**
 * @file link_lvgl.h
 * @brief LVGL runtime for the link base board.
 *
 * Step 3 of the display bring-up, after link_display.h and
 * link_touch.h. Owns the LVGL library, its draw buffers, the display
 * and input drivers, the tick source and the handler task.
 *
 * Expected sequence:
 *   link_display_panel_init();
 *   link_touch_init();          // optional, no indev without it
 *   link_lvgl_init();
 *   link_lvgl_lock(UINT32_MAX);
 *   ... build screens ...
 *   link_lvgl_unlock();
 *   link_display_backlight_on();
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start LVGL and take ownership of the panel.
 *
 * Registers the display driver on link_display_panel() and, if
 * link_touch_init() has run, an input driver on the touch controller.
 * Starts the tick timer and the handler task.
 *
 * From this point the panel belongs to LVGL: link_display_fill() stops
 * working and returns ESP_ERR_INVALID_STATE. The backlight is not
 * touched - switch it on once a first screen has been drawn.
 */
esp_err_t link_lvgl_init(void);

/** @brief True once link_lvgl_init() has succeeded. */
bool link_lvgl_is_running(void);

/**
 * @brief Size of one draw buffer.
 *
 * There are two of them, in internal DMA-capable RAM. Reported by
 * "disp status" so the figure is read from the code that allocates it
 * rather than restated by the caller. Either pointer may be NULL.
 *
 * @param pixels Receives the pixel count.
 * @param bytes  Receives the size in bytes.
 */
void link_lvgl_buffer_info(unsigned *pixels, unsigned *bytes);

/**
 * @brief Copy the frame currently on the panel.
 *
 * RGB565, MSB first, rows top to bottom, LINK_DISPLAY_H_RES x
 * LINK_DISPLAY_V_RES. Takes the LVGL lock for the copy only.
 *
 * @param dst  Destination, at least H_RES * V_RES * 2 bytes.
 * @param size Size of dst in bytes.
 * @return false if CONFIG_LINK_SCREENSHOT is off, the frame could not be
 *         allocated, dst is too small or the lock timed out.
 */
bool link_lvgl_frame_copy(void *dst, size_t size);

/**
 * @brief Take the LVGL lock.
 *
 * LVGL is not thread-safe and its handler runs in its own task. Any
 * call into lv_* from another task - the console, a worker - must hold
 * this lock. Code already running inside an lv_timer callback is on the
 * handler task and must not take it.
 *
 * @param timeout_ms Wait in milliseconds, or UINT32_MAX to block.
 * @return true if the lock was taken; nothing may touch lv_* if false.
 */
bool link_lvgl_lock(uint32_t timeout_ms);

/** @brief Release the LVGL lock. */
void link_lvgl_unlock(void);

#ifdef __cplusplus
}
#endif
