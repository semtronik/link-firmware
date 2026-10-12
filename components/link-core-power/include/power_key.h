/**
 * @file power_key.h
 * @brief Power latch and the physical off button.
 *
 * The latch is held from app_main() before anything else runs; on
 * battery it is the only thing keeping the rail up once the finger
 * leaves the button.
 *
 * A long press does not cut power. It raises a request that the LVGL
 * side picks up to open the shutdown confirmation; the cut happens when
 * the user validates it.
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** How long the button must be held for the confirmation to appear. */
#define POWER_KEY_HOLD_MS 3000U

/**
 * @brief Hold the power latch.
 *
 * Must be the first statement in app_main(), before any delay.
 */
void power_key_latch(void);

/** @brief Milliseconds between power-on and the latch being asserted. */
unsigned power_key_latch_delay_ms(void);

/**
 * @brief Start watching the button.
 *
 * The watcher only sets a flag - it never touches lv_*. LVGL is not
 * thread-safe and this runs on its own task.
 */
void power_key_start(void);

/**
 * @brief Take the pending long-press, if there is one.
 *
 * Polled from an lv_timer, which runs on the LVGL task and may therefore
 * open the overlay directly. Clears the request, so it reports true once
 * per press.
 */
bool power_key_take_request(void);

/**
 * @brief Drop the latch.
 *
 * Logs before moving the pin: once PWR_HOLD is low the rail is on its
 * way out and nothing after it is guaranteed to reach the console. With
 * USB plugged in the charger holds the latch regardless and this
 * returns.
 */
void power_key_power_off(const char *why);

/** @brief Called by power_key_power_off() before anything else; NULL to remove. */
void power_key_set_before_off(void (*cb)(const char *why));

#ifdef __cplusplus
}
#endif
