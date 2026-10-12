/**
 * @file link_time.h
 * @brief Wall-clock time, kept in step with the RTC.
 *
 * The PCF8563 has a crystal and a backup cell; the ESP32's own clock has
 * neither, and drifts seconds a day. So the RTC is the reference, and
 * the system clock is a cache of it: seeded at boot, re-read every few
 * minutes. Everything else calls gettimeofday() and never touches I2C.
 *
 * Timestamps taken from here are epoch milliseconds. A frame records
 * one when it arrives and keeps it: a later re-sync moves the clock, it
 * does not rewrite what was already stamped.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Seed the system clock from the RTC.
 *
 * Call after board_core_bus_init(). An RTC that lost power is not an
 * error: the clock is left where it is and link_time_is_valid() stays
 * false, which is what the screens show as "a regler".
 */
esp_err_t link_time_init(void);

/**
 * @brief Re-read the RTC if enough time has passed since the last one.
 *
 * Cheap to call often - it only reaches the bus every few minutes.
 *
 * Must not be called from the LVGL task: it touches the core I2C bus,
 * and a device that does not answer costs a 100 ms timeout that would
 * stall the display. link_power's sampling task calls it.
 */
void link_time_tick(void);

/** @brief True once the RTC has given a time it vouches for. */
bool link_time_is_valid(void);

/**
 * @brief Leave the RTC with no valid time, for a factory reset.
 *
 * Writes a date before LINK_TIME_VALID_FROM_YEAR, which every later read
 * refuses, rather than relying on the chip letting its low-voltage flag
 * be set by hand.
 */
esp_err_t link_time_invalidate(void);

/** Earliest year the clock accepts as set, as set_time does. */
#define LINK_TIME_VALID_FROM_YEAR 2024

/**
 * @brief Write a time to the RTC and to the system clock.
 *
 * The RTC first: it is the reference, and a write is what clears its
 * low-voltage flag.
 */
esp_err_t link_time_set(const struct tm *local);

/*
 * UTC is what is stored and exchanged; the local offset is a display
 * setting kept in NVS. The app sets both, the screens only ever see
 * local time.
 */

/** @brief Current UTC, epoch seconds. */
time_t link_time_get_utc(void);

/** @brief Set UTC, writing the RTC and the system clock. */
esp_err_t link_time_set_utc(time_t utc);

/** @brief Called before every set, from the device or the app; NULL to remove. */
void link_time_set_before_set(void (*cb)(void));

/** @brief Local offset in minutes, e.g. 120 for UTC+2. */
int16_t link_time_get_offset_minutes(void);

/** @brief Set and persist the local offset. */
esp_err_t link_time_set_offset_minutes(int16_t minutes);

/** @brief Current local time, broken down. */
void link_time_get_local(struct tm *out);

/** @brief Epoch milliseconds, for stamping. */
int64_t link_time_now_ms(void);

/**
 * @brief Format an epoch stamp as "DD/MM/YY HH:MM:SS".
 *
 * @param buf At least 18 bytes. A stamp of 0, or a clock that was never
 *            set, gives "--/--/-- --:--:--".
 */
void link_time_format_ms(int64_t ts_ms, char *buf, unsigned size);

#ifdef __cplusplus
}
#endif
