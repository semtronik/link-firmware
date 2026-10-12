/**
 * @file ui_manager.h
 * @brief Screen ownership, navigation and the refresh timer.
 *
 * Persistent screens are built once at init and swapped with
 * lv_screen_load(). Overlays - shutdown, battery alerts, the stepper and
 * the picker - are built on demand on top of whatever is active and
 * deleted on close, so they have no entry here.
 *
 * Capture is the root: it is what boot lands on and what the back stack
 * bottoms out at. The device is always receiving, so there is no neutral
 * screen to return to.
 *
 * Everything in this file runs on the LVGL task. Callers from another
 * task must go through lv_async_call().
 */

#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "link_screens.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    /* The root: boot lands here and the back stack bottoms out at it.
     * Signal, listen and events are views inside it, not screens. */
    UI_SCREEN_CAPTURE = 0,
    UI_SCREEN_MENU,
    UI_SCREEN_CONFIG,
    UI_SCREEN_SCAN,
    UI_SCREEN_ABOUT,
    UI_SCREEN_SYSINFO,
    UI_SCREEN_SYSTEM,
    UI_SCREEN_DATETIME,
    UI_SCREEN_COUNT,
} ui_screen_id_t;

/**
 * @brief Build the screens and start refreshing them.
 *
 * link_theme_init() must have run first - the styles are attached to the
 * objects built here and re-initialising them afterwards invalidates
 * what is already on screen.
 */
esp_err_t ui_manager_init(void);

/*
 * app_ui_rebuild() and app_ui_navigate_back() are declared in app_ui.h:
 * they are what the shared screens require from this file.
 */

/** @brief Switch to a persistent screen, pushing the current one. */
void ui_manager_switch_to(ui_screen_id_t screen);

/**
 * @brief Go straight back to the capture, from anywhere.
 *
 * The stack is emptied rather than unwound: the capture is the root, so
 * there is nothing left below it to return to.
 */
void ui_manager_go_home(void);

/** @brief Which persistent screen is loaded. */
ui_screen_id_t ui_manager_active_screen(void);

/*
 * Product screens. The shared ones - about, sysinfo, system, datetime,
 * the overlays - are declared in link_screens.h, which this file
 * includes.
 */
lv_obj_t *ui_screen_menu_create(void);
lv_obj_t *ui_screen_capture_create(void);
lv_obj_t *ui_screen_config_create(void);
lv_obj_t *ui_screen_scan_create(void);

void ui_screen_menu_refresh(void);
void ui_screen_capture_refresh(void);
void ui_screen_config_refresh(void);
void ui_screen_scan_refresh(void);

/**
 * @brief Start-up setup over @p parent: language, region and clock when
 *        @p full, the clock alone otherwise.
 *
 * Blocks until it is done; call without the LVGL lock, before
 * ui_manager_init(). Leaves a blank cover on @p parent, to be deleted
 * with it.
 */
void ui_screen_setup_run(lv_obj_t *parent, bool full);

/** @brief Add the radio rows to the shared system-state screen. */
void ui_sysinfo_radio_register(void);

/**
 * @brief Begin a session, if one is not already running.
 *
 * Called at boot: the device receives from the moment it is on, so a
 * session is not something one enters. Idempotent.
 */
void ui_session_start(void);

/** @brief End the running session, if any. */
void ui_session_stop(void);

#ifdef __cplusplus
}
#endif
