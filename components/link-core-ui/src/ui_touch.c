#include "ui_touch.h"

#include <stdlib.h>

#include "esp_log.h"

static const char *TAG = "ui_touch";

/* Checked on both axes, so vertical scrolls and horizontal swipes are
 * both rejected. See ui_touch.h for why this value is what it is. */
#define DRAG_REJECT_PX UI_TOUCH_DRAG_REJECT_PX

/* Shorter than this is a contact artefact, not a finger. */
#define MIN_TAP_MS 50

/* Per button, hung off the button itself: globals here break as soon as
 * two buttons see contact in the same gesture. */
typedef struct {
    ui_touch_tap_cb_t on_tap;
    void             *user_data;
    lv_point_t        press_pt;
    bool              drag_detected;
    uint32_t          press_tick;
} touch_state_t;

/* Runs once, when the button is destroyed - never in steady state. */
static void state_free_cb(lv_event_t *e)
{
    free(lv_event_get_user_data(e));
}

static void press_cb(lv_event_t *e)
{
    touch_state_t *st = lv_event_get_user_data(e);
    lv_indev_t *indev = lv_indev_active();

    if (indev != NULL) {
        lv_indev_get_point(indev, &st->press_pt);
    }
    st->drag_detected = false;
    st->press_tick = lv_tick_get();
}

static void pressing_cb(lv_event_t *e)
{
    touch_state_t *st = lv_event_get_user_data(e);

    if (st->drag_detected) {
        return;
    }

    lv_indev_t *indev = lv_indev_active();
    if (indev == NULL) {
        return;
    }

    lv_point_t p;
    lv_indev_get_point(indev, &p);

    const int32_t dx = LV_ABS(p.x - st->press_pt.x);
    const int32_t dy = LV_ABS(p.y - st->press_pt.y);

    /* Sticky for the rest of the gesture: a finger that drifts and
     * comes back is still not a tap. */
    if (dx > DRAG_REJECT_PX || dy > DRAG_REJECT_PX) {
        st->drag_detected = true;
    }
}

static void clicked_cb(lv_event_t *e)
{
    touch_state_t *st = lv_event_get_user_data(e);

    if (st->drag_detected) {
        ESP_LOGD(TAG, "rejected: drag");
        return;
    }

    lv_indev_t *indev = lv_indev_active();
    if (indev != NULL && lv_indev_get_scroll_obj(indev) != NULL) {
        ESP_LOGD(TAG, "rejected: scrolling");
        return;
    }

    const uint32_t held = lv_tick_elaps(st->press_tick);
    if (held < MIN_TAP_MS) {
        ESP_LOGD(TAG, "rejected: %lums contact", (unsigned long)held);
        return;
    }

    /* The callback is about to change what is under the finger. */
    ui_touch_wait_release_all();

    if (st->on_tap != NULL) {
        st->on_tap(st->user_data);
    }
}

lv_obj_t *ui_touch_make_tap_button(lv_obj_t *parent,
                                   ui_touch_tap_cb_t on_tap,
                                   void *user_data)
{
    touch_state_t *st = calloc(1, sizeof(*st));
    if (st == NULL) {
        return NULL;
    }
    st->on_tap = on_tap;
    st->user_data = user_data;

    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_add_event_cb(btn, press_cb, LV_EVENT_PRESSED, st);
    lv_obj_add_event_cb(btn, pressing_cb, LV_EVENT_PRESSING, st);
    lv_obj_add_event_cb(btn, clicked_cb, LV_EVENT_CLICKED, st);
    lv_obj_add_event_cb(btn, state_free_cb, LV_EVENT_DELETE, st);

    return btn;
}

lv_obj_t *ui_touch_make_scroll_safe_button(lv_obj_t *parent,
                                           ui_touch_tap_cb_t on_tap,
                                           void *user_data)
{
    return ui_touch_make_tap_button(parent, on_tap, user_data);
}

void ui_touch_tune_indevs(void)
{
    lv_indev_t *indev = NULL;

    while ((indev = lv_indev_get_next(indev)) != NULL) {
        if (lv_indev_get_type(indev) != LV_INDEV_TYPE_POINTER) {
            continue;
        }

        lv_indev_set_gesture_min_distance(indev, UI_TOUCH_GESTURE_DISTANCE_PX);
        lv_indev_set_gesture_min_velocity(indev, UI_TOUCH_GESTURE_VELOCITY_PX);
        lv_indev_set_scroll_limit(indev, UI_TOUCH_SCROLL_LIMIT_PX);
    }
}

void ui_touch_wait_release_all(void)
{
    for (lv_indev_t *indev = lv_indev_get_next(NULL); indev != NULL;
         indev = lv_indev_get_next(indev)) {
        lv_indev_wait_release(indev);
    }
}
