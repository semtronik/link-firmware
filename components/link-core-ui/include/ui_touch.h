/**
 * @file ui_touch.h
 * @brief Touch helpers - drag rejection and phantom-click guards.
 *
 * Screens create their buttons through this file rather than calling
 * lv_button_create() directly: a raw LVGL button on a resistive-feeling
 * capacitive panel fires on scrolls and on contact artefacts, and the
 * guards against that belong in one place.
 */

#pragma once

#include <stdbool.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The three thresholds that decide what a contact turns into. They are
 * defined together because only their ORDER makes the screens work, and
 * that order is not obvious from any one of them:
 *
 *   gesture (8) < scroll (10) <= drag reject (10)
 *
 * Why the gesture must come first: LVGL's indev_gesture() opens with
 * "if(indev->pointer.scroll_obj) return;". Once a scroll object has
 * been picked, no gesture is ever evaluated again for that contact. A
 * swipe is never perfectly horizontal, and find_scroll_obj() decides
 * the axis by comparing the running sums on the first few reads - a few
 * degrees of slant is enough for LVGL to call a swipe vertical, latch
 * onto a scrollable list and swallow the whole gesture. Deciding the
 * gesture below the scroll threshold settles the race.
 *
 * Why the drag reject must not come first: below LVGL's scroll limit
 * the button is still shown pressed, so rejecting a tap any earlier
 * lights a button up and then does nothing when the finger lifts.
 *
 * The scroll limit is LVGL's own (LV_INDEV_DEF_SCROLL_LIMIT). It is set
 * explicitly in ui_touch_tune_indevs() rather than inherited, so this
 * file states all three rather than assuming one of them.
 */
#define UI_TOUCH_GESTURE_DISTANCE_PX 8
#define UI_TOUCH_SCROLL_LIMIT_PX     10
#define UI_TOUCH_DRAG_REJECT_PX      10

/* Below this, a slow deliberate drag keeps zeroing its own running
 * total and never registers. LVGL's default of 3 is a flick. */
#define UI_TOUCH_GESTURE_VELOCITY_PX 1

_Static_assert(UI_TOUCH_GESTURE_DISTANCE_PX < UI_TOUCH_SCROLL_LIMIT_PX,
               "the gesture must be decided before LVGL picks a scroll "
               "object, or swipes over a scrollable list are swallowed");

_Static_assert(UI_TOUCH_DRAG_REJECT_PX >= UI_TOUCH_SCROLL_LIMIT_PX,
               "rejecting a tap before LVGL drops the pressed state "
               "leaves a button lit up that then does nothing");

/**
 * @brief Apply the thresholds above to every pointer device.
 *
 * Call once, after the display and input driver are up. Without it the
 * scroll limit is LVGL's default and the ordering above is a guess.
 */
void ui_touch_tune_indevs(void);

/** @brief Called on a tap that survived the guards. */
typedef void (*ui_touch_tap_cb_t)(void *user_data);

/**
 * @brief Create a button that only fires on a genuine tap.
 *
 * Rejected: a press that drifts more than the drag threshold, a press
 * LVGL has escalated into a scroll, and contact shorter than a real
 * finger tap. Every indev is waited out before the callback runs, so
 * the release cannot land on whatever the callback puts on screen.
 *
 * The button comes back unstyled and unsized - the caller owns its
 * appearance and its children.
 */
lv_obj_t *ui_touch_make_tap_button(lv_obj_t *parent,
                                   ui_touch_tap_cb_t on_tap,
                                   void *user_data);

/**
 * @brief Same, for a button inside a scrollable container.
 *
 * Currently identical to ui_touch_make_tap_button(): the guards above
 * already cover the scroll case. Kept as its own entry point because
 * the call sites mean something different by it.
 */
lv_obj_t *ui_touch_make_scroll_safe_button(lv_obj_t *parent,
                                           ui_touch_tap_cb_t on_tap,
                                           void *user_data);

/**
 * @brief Block until every input device is released.
 *
 * Call before anything that changes what is under the finger - opening
 * or closing an overlay, switching screens - or the release fires on
 * the new content.
 */
void ui_touch_wait_release_all(void);

#ifdef __cplusplus
}
#endif
