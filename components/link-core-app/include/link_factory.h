/**
 * @file link_factory.h
 * @brief Factory reset: the device as it left the bench, firmware aside.
 *
 * Clears the clock, the application's own data, the event log and the
 * whole NVS partition, then restarts. The next boot is a first boot.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief What the application erases of its own, e.g. its recordings.
 *
 * Called after the clock is cleared and before the platform's data goes.
 * NULL when the application keeps nothing.
 */
void link_factory_set_app_erase(void (*cb)(void));

/** @brief Erase everything and restart. Does not return. */
void link_factory_reset(void);

#ifdef __cplusplus
}
#endif
