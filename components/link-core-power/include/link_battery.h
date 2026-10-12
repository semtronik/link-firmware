/**
 * @file link_battery.h
 * @brief MAX17048 fuel gauge and battery presence detection.
 *
 * The gauge is powered from +VBAT, which makes a missing cell hard to
 * see: with nothing fitted, the charger's battery-detect cycles the
 * rail, so the gauge is alive for part of the cycle and unpowered for
 * the rest. A single reading is therefore worthless - it may be a
 * perfectly plausible voltage taken during the half of the cycle where
 * the rail happens to be up.
 *
 * Detection here is passive: link_battery_poll() is called periodically
 * by whoever already refreshes the power state, and the verdict falls
 * out on its own. Nothing blocks, and no caller waits.
 *
 * Expected sequence:
 *   board_core_bus_init();
 *   link_battery_init();
 *   ... every second or so ...
 *   link_battery_poll(usb_present);
 *   link_battery_get(&state);
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 7-bit address of the MAX17048. Fixed, no address pins. */
#define LINK_BATTERY_ADDR 0x36

typedef enum {
    /** Not enough observations yet. The only state at startup. */
    LINK_BATTERY_UNKNOWN = 0,
    /** A cell is fitted. */
    LINK_BATTERY_PRESENT,
    /** No cell: the charger is cycling an open output. */
    LINK_BATTERY_ABSENT,
} link_battery_presence_t;

typedef struct {
    link_battery_presence_t presence;
    /** false: the gauge did not answer, the fields below are stale. */
    bool gauge_ok;
    uint32_t vcell_mv;
    uint8_t soc_pct;
    /** Charge (positive) or discharge (negative) rate, milli-%/h. */
    int32_t crate_milli_pct_h;
} link_battery_t;

/**
 * @brief Reset the detector and probe the gauge once.
 *
 * The bus must already be up. A gauge that does not answer here is not
 * an error: with no cell fitted it is unpowered for part of every
 * detect cycle, which is exactly what this module is here to notice.
 */
esp_err_t link_battery_init(void);

/**
 * @brief Take one sample and advance the detector.
 *
 * Cheap: one or three register reads, no delay, no retry. Call it at
 * whatever rate the caller already refreshes at - roughly 1 Hz is
 * plenty, and slower still works, it only makes the verdict later.
 *
 * @param usb_present PRES_USB, as read by the caller. With USB out, the
 *                    board is running off the cell, which settles the
 *                    question immediately - see the note in the .c file.
 */
void link_battery_poll(bool usb_present);

/**
 * @brief Latest reading and verdict.
 *
 * @param out Receives the state. Never fails; before the first poll it
 *            reports UNKNOWN with gauge_ok false.
 */
void link_battery_get(link_battery_t *out);

/** @brief Human-readable presence, for logs and screens. */
const char *link_battery_presence_str(link_battery_presence_t presence);

#ifdef __cplusplus
}
#endif
