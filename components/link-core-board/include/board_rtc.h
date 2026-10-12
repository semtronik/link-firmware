/**
 * @file board_rtc.h
 * @brief PCF8563 real-time clock on the core I2C bus.
 */

#pragma once

#include <stdbool.h>
#include <time.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 7-bit address of the PCF8563. Fixed, no address pins. */
#define BOARD_RTC_ADDR 0x51

/**
 * @brief Read the current date and time.
 *
 * @param out      Receives the decoded time. tm_isdst is set to -1.
 * @param low_volt Receives the VL flag: true means the RTC lost power
 *                 since the last time it was set, so @p out is stale and
 *                 must not be trusted. Never NULL - the flag is the whole
 *                 point of reading this chip on a battery-backed board.
 *
 * @return ESP_OK on a successful transfer, or an error from the I2C
 *         layer. A successful read with VL set is still ESP_OK: the
 *         transfer worked, the value is simply meaningless.
 */
esp_err_t board_rtc_get_time(struct tm *out, bool *low_volt);

/**
 * @brief Set the date and time, and clear the VL flag.
 *
 * Clearing VL is the point: it is what marks the stored time as
 * trustworthy from now on, and it only ever clears on a write.
 *
 * @param t Time to store. Only the calendar fields are used.
 */
esp_err_t board_rtc_set_time(const struct tm *t);

/** @brief Frequency the CLKOUT pin carries. */
typedef enum {
    BOARD_RTC_CLKOUT_OFF,      /**< Output disabled, pin high-impedance. */
    BOARD_RTC_CLKOUT_32768HZ,  /**< The power-on default. */
    BOARD_RTC_CLKOUT_1024HZ,
    BOARD_RTC_CLKOUT_32HZ,
    BOARD_RTC_CLKOUT_1HZ,      /**< Slow enough to time drift by hand. */
} board_rtc_clkout_t;

/**
 * @brief Choose what CLKOUT carries.
 *
 * The chip enables CLKOUT at 32.768 kHz on power-up, so this call is
 * only ever a change of mind - never what starts the output.
 *
 * On this board CLKOUT goes to TP11 and nowhere else, and the pin is
 * open-drain with no pull-up fitted: reading it needs a resistor to
 * 3V3 across the test pad. Without one the pin floats and a scope sees
 * nothing, whatever this function was asked for.
 *
 * @param freq Frequency to output, or BOARD_RTC_CLKOUT_OFF.
 */
esp_err_t board_rtc_set_clkout(board_rtc_clkout_t freq);

#ifdef __cplusplus
}
#endif
