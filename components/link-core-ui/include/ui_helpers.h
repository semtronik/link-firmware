/**
 * @file ui_helpers.h
 * @brief Structural patterns shared by the screens.
 *
 * Any visual pattern used by two screens belongs here. Copy-pasting
 * structure between screens means a helper is missing - add it.
 */

#pragma once

#include <stdbool.h>

#include "lvgl.h"
#include "link_theme.h"
#include "ui_touch.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Row grammars. Which one a row uses tells the user what tapping it
 * will do, so they are not interchangeable:
 *
 *   NAV  - icon, label, chevron. "This opens a sub-screen."
 *   EDIT - label, current value, chevron. "This edits the value."
 *   READ - label, value, no chevron, no press feedback, not tappable.
 */
typedef enum {
    UI_LIST_ROW_NAV,
    UI_LIST_ROW_EDIT,
    UI_LIST_ROW_READ,
} ui_list_row_kind_t;

typedef struct {
    ui_list_row_kind_t kind;
    const char *icon;  /**< LV_SYMBOL_*, NAV only */
    const char *label; /**< required */
    const char *value; /**< EDIT and READ */
} ui_list_row_spec_t;

/** @brief Scrollable vertical container with a visible scrollbar. */
lv_obj_t *ui_helpers_create_list_container(lv_obj_t *parent);

/**
 * @brief One row, built to the grammar in @p spec.
 *
 * @param out_value_label Receives the value label so the caller can
 *                        update it later. NULL for NAV rows.
 */
lv_obj_t *ui_helpers_create_list_row(lv_obj_t *container,
                                     const ui_list_row_spec_t *spec,
                                     ui_touch_tap_cb_t on_tap,
                                     void *user_data,
                                     lv_obj_t **out_value_label);

/**
 * @brief Flex column filling the standard content area.
 *
 * Rows go in through ui_helpers_add_list_row(), which sizes them. No
 * positioning or height arithmetic on the screen side.
 */
lv_obj_t *ui_helpers_create_flex_list(lv_obj_t *parent);

/** @brief Add a row to a flex list, at the standard width and height. */
lv_obj_t *ui_helpers_add_list_row(lv_obj_t *flex_list,
                                  const ui_list_row_spec_t *spec,
                                  ui_touch_tap_cb_t on_tap,
                                  void *user_data,
                                  lv_obj_t **out_value_label);

/** @brief Header button - back, menu, validate. 48x48. */
lv_obj_t *ui_helpers_create_header_button(lv_obj_t *parent,
                                          const char *symbol,
                                          ui_touch_tap_cb_t on_tap,
                                          void *user_data);

/**
 * @brief Header bar for a full-screen overlay: back, title, and an
 *        optional validate button.
 *
 * Pass NULL for @p on_back or @p on_validate to leave that slot empty.
 *
 * @return The title label.
 */
lv_obj_t *ui_helpers_attach_overlay_header(lv_obj_t *overlay,
                                           const char *title,
                                           ui_touch_tap_cb_t on_back,
                                           void *back_user_data,
                                           ui_touch_tap_cb_t on_validate,
                                           void *validate_user_data);

/*
 * Monitor status bar - the one screen allowed to depart from the
 * standard header.
 *
 *   [menu] Monitor      o o      14:32 [batt]
 *
 * The menu button and the screen's name on the left, the page dots
 * centred, and the clock with a battery icon on the right.
 *
 * The percentage is deliberately not here: the icon says enough at a
 * glance, and the number lives in the system info screen.
 */
/* A request for more than this is dropped whole - no pips at all. */
#define UI_STATUS_DOTS_MAX 8

typedef struct {
    /**
     * The bar itself, so the pips can be parented to it directly.
     *
     * Kept rather than walked up to: the clock sits inside nested flex
     * containers, and a pip parented to one of those is re-placed by
     * the flex, which throws away the x that centres it.
     */
    lv_obj_t *bar;
    /** The screen's name, so a bar whose title is a value can rewrite
     *  it. NULL when the bar was built without one. */
    lv_obj_t *lbl_title;
    /** Row 1 of the right-hand block, above the time. */
    lv_obj_t *lbl_date;
    lv_obj_t *lbl_time;
    lv_obj_t *icon_battery;
    /** Bolt over the battery icon, hidden unless charging. */
    lv_obj_t *icon_charging;
    lv_obj_t *btn_menu;
    /** Page indicator pips, centred in the bar; NULL when none added. */
    lv_obj_t *dots[UI_STATUS_DOTS_MAX];
    int dot_count;
    /** Left edge of the pip row, so set_page can lay it out again. */
    int32_t pip_start_x;
} ui_status_bar_handles_t;

/**
 * @param title Screen name, centred. NULL leaves the slot empty.
 */
void ui_helpers_create_status_bar(lv_obj_t *screen,
                                  const char *title,
                                  ui_touch_tap_cb_t on_menu,
                                  void *menu_user_data,
                                  ui_status_bar_handles_t *out_handles);

/**
 * @brief Repaint the clock and the battery icon.
 *
 * Call from whichever screen owns the bar, on its refresh. Reads the
 * published power snapshot, never the bus.
 */
void ui_helpers_update_status_bar(const ui_status_bar_handles_t *handles);

/**
 * @brief Add page dots to the centre of the status bar.
 *
 * Markers only - no touch target, so they cost nothing but their own
 * 8 px and fit in the band beside the clock. For a screen whose pages
 * are reached by swiping and whose footer changes from one view to the
 * next, which is what stops the dots from living there.
 *
 * Call once, after ui_helpers_create_status_bar(), on the same handles.
 */
void ui_helpers_status_bar_add_dots(ui_status_bar_handles_t *handles,
                                    int count,
                                    int initial_index);

/** @brief Move the highlight, for when a swipe changed the page. */
void ui_helpers_status_bar_set_page(const ui_status_bar_handles_t *handles,
                                    int index);

/**
 * @brief Row of page-position dots, pinned to the bottom.
 *
 * Markers, not navigation: swiping moves between pages and tapping a
 * dot is the alternative. Each dot carries an invisible 44x44 target so
 * it can be hit; tapping the active one does nothing.
 */
typedef void (*ui_pager_change_cb_t)(int index, void *user_data);

lv_obj_t *ui_helpers_create_pager_dots(lv_obj_t *parent,
                                       int count,
                                       int initial_index,
                                       ui_pager_change_cb_t on_change,
                                       void *user_data);

/** @brief Move the highlight, for when a swipe changed the page. */
void ui_helpers_pager_set_active(lv_obj_t *pager, int index);

/*
 * Signal block - a big value, a small bar under it, a caption. Used for
 * RSSI and SNR, which have different scales and different thresholds;
 * the type is what selects them.
 */
typedef enum {
    UI_SIGNAL_RSSI,
    UI_SIGNAL_SNR,
} ui_signal_type_t;

typedef struct {
    lv_obj_t *block;
    lv_obj_t *lbl_value;
    lv_obj_t *lbl_unit;
    lv_obj_t *bar_bg;
    lv_obj_t *bar_fill;
    lv_obj_t *lbl_caption;
    ui_signal_type_t type;
} ui_signal_block_t;

void ui_helpers_create_signal_value(lv_obj_t *parent,
                                    const char *caption,
                                    const char *unit,
                                    ui_signal_type_t type,
                                    ui_signal_block_t *out_block);

/**
 * @brief Update the value, its colour and the bar.
 *
 * @param has_value false shows "--" and an empty bar: nothing has been
 *                  received yet, which is not the same as a bad signal.
 */
void ui_helpers_update_signal_value(ui_signal_block_t *block,
                                    float value,
                                    bool has_value);

/** @brief The colour a reading takes wherever it is shown. */
lv_color_t ui_helpers_signal_color(ui_signal_type_t type, float value);

/*
 * Stepper overlay - [-] value [+] with a back/validate header. The
 * component owns the overlay, the layout, the debounce and the
 * teardown; the caller supplies the range and the callbacks.
 */
typedef void (*ui_stepper_commit_cb_t)(int value);
typedef void (*ui_stepper_cancel_cb_t)(void);
typedef void (*ui_stepper_preview_cb_t)(int value);

/** @brief Render a value that a printf format cannot express on its own. */
typedef void (*ui_stepper_format_cb_t)(int value, char *buf, unsigned size);

typedef struct {
    const char *title;
    const char *format; /**< printf format for the value, e.g. "%d %%" */
    /** Takes precedence over @c format when set - for a value that is
     *  not one number, such as minutes shown as "UTC+2:00". */
    ui_stepper_format_cb_t format_cb;
    int initial;
    int min;
    int max;
    int step;
    ui_stepper_commit_cb_t on_commit;   /**< [OK], with the final value */
    ui_stepper_cancel_cb_t on_cancel;   /**< [back], optional */
    ui_stepper_preview_cb_t on_preview; /**< every step, optional */
} ui_stepper_cfg_t;

void ui_stepper_open(const ui_stepper_cfg_t *cfg);
bool ui_stepper_is_open(void);

/*
 * List picker overlay - radio list with a back/validate header.
 */
typedef void (*ui_list_picker_commit_cb_t)(int index);
typedef void (*ui_list_picker_cancel_cb_t)(void);

typedef struct {
    const char *title;
    const char **labels; /**< must outlive the overlay */
    int count;
    int initial;
    ui_list_picker_commit_cb_t on_commit;
    ui_list_picker_cancel_cb_t on_cancel; /**< optional */
} ui_list_picker_cfg_t;

void ui_list_picker_open(const ui_list_picker_cfg_t *cfg);
bool ui_list_picker_is_open(void);

#ifdef __cplusplus
}
#endif
