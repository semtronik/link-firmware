/**
 * @file link_power.h
 * @brief Charge state, from the charger rather than from the gauge.
 *
 * link_battery tells how full the cell is; this says what is happening
 * to it. The two answer different questions and come from different
 * chips: the level from the MAX17048 over I2C, the charge state from
 * the BQ24074's status pin.
 *
 * Sampling is internal: link_power_init() starts the task that reads
 * both, advances the battery detector underneath, and writes the USB
 * and charge lines to the event log. Callers only read the result.
 */

#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "link_battery.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    /** No USB: the board is running off the cell. */
    LINK_POWER_DISCHARGING,
    /** USB in, charger pulling its status pin low. */
    LINK_POWER_CHARGING,
    /** USB in, charge finished or not started. */
    LINK_POWER_CHARGED,
    /** USB in with no cell fitted. */
    LINK_POWER_NO_BATTERY,
} link_power_state_t;

typedef struct {
    link_power_state_t state;
    bool usb_present;
    /** The charger's ~CHG pin, already un-inverted. */
    bool charging;
    link_battery_t battery;
} link_power_t;

/**
 * @brief Configure PRES_USB and ~CHG, then start the sampling task.
 *
 * The bus must already be up. Sampling runs on its own task rather than
 * on whatever timer happens to refresh the screens: a gauge that does
 * not answer costs one I2C timeout per register, and three of those on
 * the LVGL thread stall the display for the best part of a second.
 */
esp_err_t link_power_init(void);

/** @brief Latest state. Never fails. */
void link_power_get(link_power_t *out);

/** @brief Human-readable state, for logs and screens. */
const char *link_power_state_str(link_power_state_t state);

#ifdef __cplusplus
}
#endif
