/**
 * @file ui_view.h
 * @brief The box a capture page builds into.
 *
 * The capture owns the header, the pips, the action bar and the gesture
 * that moves between pages. A page draws its content and nothing else, at
 * (0,0) of a box VIEW_CONTENT_W by VIEW_CONTENT_H.
 */

#pragma once

#include <stdbool.h>

#include "link_theme.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* What the header takes at the top and the action bar at the foot. */
#define VIEW_CONTENT_W LINK_USABLE_W
#define VIEW_CONTENT_H (LINK_MONITOR_CONTENT_H - LINK_FOOTER_H)

/* A sheet's body: the whole screen under its title. */
#define VIEW_SHEET_H (LINK_SCREEN_H - LINK_HEADER_H - LINK_PAD_SM)

typedef void (*ui_view_sheet_fill_t)(lv_obj_t *body);

/**
 * Opens a sheet over the pages, titled and with a back arrow, as the frame
 * detail does. The page fills body, VIEW_CONTENT_W by VIEW_SHEET_H; closed
 * is called once the sheet has gone. False if one is already open.
 */
bool ui_view_open_sheet(const char *title, ui_view_sheet_fill_t fill,
                        void (*closed)(void));

/** The session's report. Positioned by the caller, like every page. */
lv_obj_t *ui_view_stats_create(lv_obj_t *parent);

/** Called only while the page is in front. */
void ui_view_stats_refresh(void);

#ifdef __cplusplus
}
#endif
