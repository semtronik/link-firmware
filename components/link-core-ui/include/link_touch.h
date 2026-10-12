/**
 * @file link_touch.h
 * @brief FT5426 capacitive touch bring-up for the link base board.
 *
 * Step 2 of the display bring-up, after link_display.h. The controller
 * itself is handled by the managed component esp_lcd_touch_ft5x06; this
 * layer only wires it to the board pins and rotates its output to match
 * the landscape panel.
 *
 * Expected sequence:
 *   link_touch_init();
 *   while (...) { if (link_touch_read(&x, &y)) { ... } }
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_lcd_touch.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Bring up the touch I2C bus and the FT5426 controller.
 *
 * Claims CTP_RESET and CTP_INT: the driver drives the reset pulse
 * itself, so nothing else may touch those pins afterwards.
 */
esp_err_t link_touch_init(void);

/**
 * @brief Poll the controller for a single contact point.
 *
 * Coordinates are already rotated into the panel's landscape frame, so
 * they share the origin and the axes of link_display_fill().
 *
 * @param x Receives the X coordinate, 0..LINK_DISPLAY_H_RES-1.
 * @param y Receives the Y coordinate, 0..LINK_DISPLAY_V_RES-1.
 * @return true if a finger is down, false otherwise.
 */
bool link_touch_read(uint16_t *x, uint16_t *y);

/**
 * @brief Underlying touch handle, or NULL before init.
 *
 * Exposed so the LVGL layer can attach to it later.
 */
esp_lcd_touch_handle_t link_touch_handle(void);

/**
 * @brief Called from the CTP_INT ISR on every controller pulse.
 *
 * Runs in interrupt context: it must not block, log or touch I2C. The
 * LVGL layer installs one to wake its task, which is what makes the
 * touch path interrupt-driven rather than polled.
 *
 * @param arg Opaque pointer handed back to the callback.
 * @return true if a higher priority task was woken, so the ISR yields.
 */
typedef bool (*link_touch_isr_cb_t)(void *arg);

/**
 * @brief Install the interrupt callback and enable the INT line.
 *
 * Call after link_touch_init(). Passing NULL removes the callback and
 * disables the interrupt.
 *
 * This FT5426 does not accept trigger mode (see link_touch.c), yet the
 * line keeps producing edges throughout a contact. The LVGL layer uses
 * them to shorten its wait, on top of a periodic read that runs
 * regardless - so the edges buy latency, and losing them would cost
 * responsiveness rather than the touch itself.
 * link_touch_isr_count() is how the trace is checked on a given board.
 */
esp_err_t link_touch_set_isr_callback(link_touch_isr_cb_t cb, void *arg);

/**
 * @brief Number of CTP_INT edges seen since boot.
 *
 * Read-only counter, incremented in the ISR. It is the direct evidence
 * that the INT trace works: it must climb while a finger is down and
 * stay put at rest.
 */
uint32_t link_touch_isr_count(void);

/** @brief ID_G_MODE as the controller reports it, -1 if unread. */
int link_touch_int_mode(void);

#ifdef __cplusplus
}
#endif
