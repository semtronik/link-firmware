/*
 * Texts of the shared screens. One id per text; the French and English
 * are side by side in screens_text.c.
 */

#pragma once

typedef enum {
    ST_BACK,
    ST_OK,

    ST_PREFERENCES,
    ST_THEME,
    ST_THEME_LIGHT,
    ST_THEME_DARK,
    ST_BACKLIGHT,
    ST_LANGUAGE,

    ST_ABOUT,
    ST_BOARD,

    ST_BATTERY_LOW,
    ST_BATTERY_LOW_HINT,
    ST_BATTERY_CRITICAL,
    ST_BATTERY_CRITICAL_HINT,

    ST_DATE_TIME,
    ST_ZONE,
    ST_DATE,
    ST_TIME,
    ST_CLOCK_STATE,
    ST_CLOCK_NOT_SET,

    ST_POWER_OFF,

    ST_FACTORY_RESET_Q,
    ST_FACTORY_RESET_HINT,
    ST_CANCEL,
    ST_ERASE,

    ST_SYSINFO,
    ST_BATTERY,
    ST_POWER,
    ST_UPTIME,
    ST_GAUGE_SILENT,
    ST_BATTERY_ABSENT,
    ST_BATTERY_DETECTING,
    ST_POWER_DISCHARGING,
    ST_POWER_CHARGING,
    ST_POWER_CHARGED,
    ST_POWER_NO_BATTERY,

    ST_ONB_TITLE,
    ST_ONB_HINT,
    ST_ONB_CURRENT,
    ST_ONB_MANUAL,
    ST_ONB_APP,
    ST_ONB_WAITING,
    ST_ONB_WAITING_HINT,

    ST_COUNT,
} st_id_t;

/** @brief Text @p id in the current language. */
const char *st(st_id_t id);
