/**
 * @file link_screens.h
 * @brief Screens every link application shares.
 *
 * Platform screens: they show the board and its settings, never a
 * module. An application picks the ones it wants, places them in its own
 * screen table, and supplies the three functions in app_ui.h.
 *
 * Persistent screens are created once and swapped with lv_screen_load().
 * Overlays are built on demand over whatever is active and deleted on
 * close.
 *
 * Everything here runs on the LVGL task.
 */

#pragma once

#include <stdbool.h>

#include "app_ui.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Persistent screens. Each returns a root object created with a NULL
 * parent - an LVGL screen, not a child.
 */
lv_obj_t *ui_screen_about_create(void);
lv_obj_t *ui_screen_sysinfo_create(void);
lv_obj_t *ui_screen_system_create(void);
lv_obj_t *ui_screen_datetime_create(void);

/* Per-screen refresh, called by the app's timer for whichever screen is
 * in front. */
void ui_screen_sysinfo_refresh(void);
void ui_screen_system_refresh(void);
void ui_screen_datetime_refresh(void);

/**
 * @brief Add an application row to the system-state screen.
 *
 * Call from the app before ui_screen_sysinfo_create(). Rows appear above
 * the platform ones, in the order registered, and the app updates the
 * returned label from its own refresh.
 *
 * The label is asked for each time the screen is built, so it follows
 * the language. Registering more than LINK_SYSINFO_APP_ROWS_MAX rows
 * drops the extras.
 */
#define LINK_SYSINFO_APP_ROWS_MAX 4

/** @brief The row's label, in the current language. */
typedef const char *(*ui_sysinfo_row_label_cb_t)(void);

/** @brief Called on every refresh, with the row's value label. */
typedef void (*ui_sysinfo_row_update_cb_t)(lv_obj_t *value);

void ui_screen_sysinfo_add_row(ui_sysinfo_row_label_cb_t label,
                               ui_sysinfo_row_update_cb_t on_refresh);

/**
 * @brief Splash screen, shown while the rest comes up.
 *
 * @param version Printed under the name, or NULL for none.
 */
lv_obj_t *ui_screen_splash_create(const char *version);

/*
 * Shutdown confirmation, opened by a long press on the power button.
 * Takes precedence over any other overlay.
 */
void ui_screen_shutdown_open(void);

/*
 * Battery overlays, pushed by the app's refresh timer. The critical one
 * has no way out: it shows its message and cuts power.
 */
void ui_screen_battery_alert_open(void);
bool ui_screen_battery_alert_is_open(void);
void ui_screen_battery_critical_open(void);
bool ui_screen_battery_critical_is_open(void);

/**
 * @brief Stop the low-battery alert coming back.
 *
 * Called by the alert's own OK button. The app re-arms it once the pack
 * is charged back above its threshold.
 */
void app_ui_battery_alert_acknowledge(void);

/*
 * Date and time editors. One overlay, two shapes: DD / MM / YYYY or
 * HH : MM. Tapping a field selects it, the keypad fills it, and a full
 * field moves on to the next one on its own.
 *
 * The callbacks run after the overlay is gone, so one editor may open
 * another - which is how the onboarding chains date into time.
 */
typedef void (*ui_time_edit_cb_t)(void);

/**
 * @brief Edit the date, keeping the current time of day.
 *
 * @param on_commit Called after the RTC write. May be NULL.
 * @param on_cancel Called on back, nothing written. May be NULL.
 */
void ui_screen_date_edit_open(ui_time_edit_cb_t on_commit,
                              ui_time_edit_cb_t on_cancel);

/** @brief Edit the time of day, keeping the current date. Seconds go to 0. */
void ui_screen_time_edit_open(ui_time_edit_cb_t on_commit,
                              ui_time_edit_cb_t on_cancel);

/** @brief True while either editor is up. */
bool ui_screen_time_edit_is_open(void);

/**
 * @brief Pick the local offset from UTC.
 *
 * Comes before setting the clock: the offset is what decides what the
 * entered time means, and choosing it afterwards moves a time the user
 * has just confirmed.
 *
 * @param on_commit Called after the offset is stored. May be NULL.
 */
void ui_screen_zone_edit_open(ui_time_edit_cb_t on_commit);

/*
 * Steps of the first-start setup. Each covers the screen, closes itself,
 * then calls on_done or on_back - from where the next step opens, so no
 * frame shows what lies underneath.
 */
typedef void (*ui_step_cb_t)(void);

/** @brief Pick the interface language. The first step: no way back. */
void ui_screen_language_step_open(ui_step_cb_t on_done);

/**
 * @brief Set the clock: by hand (zone, date, time) or from the app.
 *
 * Moves on by itself once the clock becomes valid. Opened on a clock
 * already set, as when coming back to it, it shows the time and waits
 * for the header's validate instead.
 *
 * @param on_back NULL when there is no step before: no back button.
 */
void ui_screen_time_onboarding_open(ui_step_cb_t on_done, ui_step_cb_t on_back);

/**
 * @brief Ask before a factory reset; erasing restarts the device.
 *
 * See link_factory.h for what goes.
 */
void ui_screen_factory_reset_open(void);
bool ui_screen_time_onboarding_is_open(void);

#ifdef __cplusplus
}
#endif
