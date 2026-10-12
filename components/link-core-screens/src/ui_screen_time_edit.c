/*
 * Date and time entry.
 *
 * Both are the same screen with different fields, so they share one
 * implementation: a row of tappable fields, a keypad under it, and a
 * validate button in the header.
 *
 * Typing fills the selected field and moves on once it is full, which
 * is what makes entering a date four taps rather than a dozen.
 */

#include "screens_text.h"
#include "link_screens.h"

#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "link_time.h"
#include "ui_helpers.h"
#include "link_theme.h"

static const char *TAG = "ui_time_edit";

#define MAX_FIELDS 3

typedef struct {
    st_id_t title;
    int count;
    int digits[MAX_FIELDS];
    int min[MAX_FIELDS];
    int max[MAX_FIELDS];
    int width[MAX_FIELDS];
    const char *separator; /* between fields */
} form_t;

static const form_t FORM_DATE = {
    .title = ST_DATE,
    .count = 3,
    .digits = {2, 2, 4},
    .min = {1, 1, 2024},
    .max = {31, 12, 2099},
    .width = {56, 56, 84},
    .separator = "/",
};

static const form_t FORM_TIME = {
    .title = ST_TIME,
    .count = 2,
    .digits = {2, 2},
    .min = {0, 0},
    .max = {23, 59},
    .width = {72, 72},
    .separator = ":",
};

static const form_t *s_form;
static lv_obj_t *s_overlay;
static lv_obj_t *s_field_btn[MAX_FIELDS];
static lv_obj_t *s_field_lbl[MAX_FIELDS];
static int s_values[MAX_FIELDS];
static int s_active;

/* Digits typed into the active field since it was selected. Zero means
 * the next digit replaces what is there rather than appending to it. */
static int s_typed;

static bool s_pending_commit;
static ui_time_edit_cb_t s_on_commit;
static ui_time_edit_cb_t s_on_cancel;

#define SEP_W    16
#define FIELD_H  48
#define KP_COLS  4
#define KP_ROWS  3
#define KP_KEY_H 34
#define KP_GAP   LINK_PAD_XS

#define KP_DEBOUNCE_MS 200

static const char *KP_LABELS[KP_ROWS][KP_COLS] = {
    {"1", "2", "3", LV_SYMBOL_BACKSPACE},
    {"4", "5", "6", "0"},
    {"7", "8", "9", ""},
};

static uint32_t s_kp_last_ms;

static void refresh_field(int f)
{
    /* The year is the only four-digit field; the rest are zero-padded
     * pairs. */
    if (s_form->digits[f] == 4) {
        lv_label_set_text_fmt(s_field_lbl[f], "%04d", s_values[f]);
    } else {
        lv_label_set_text_fmt(s_field_lbl[f], "%02d", s_values[f]);
    }
}

static void highlight(int f)
{
    s_active = f;
    s_typed = 0;

    for (int i = 0; i < s_form->count; i++) {
        const bool on = i == f;
        lv_obj_set_style_bg_color(s_field_btn[i],
                                  on ? LINK_COLOR_ACCENT
                                     : LINK_COLOR_SURFACE_ELEVATED,
                                  0);
        lv_obj_set_style_text_color(s_field_lbl[i],
                                    on ? LINK_COLOR_TEXT_ON_ACCENT
                                       : LINK_COLOR_TEXT,
                                    0);
    }
}

static void clamp_field(int f)
{
    if (s_values[f] < s_form->min[f]) {
        s_values[f] = s_form->min[f];
    }
    if (s_values[f] > s_form->max[f]) {
        s_values[f] = s_form->max[f];
    }
}

static void on_field_tapped(void *user_data)
{
    highlight((int)(intptr_t)user_data);
}

static void on_keypad_key(lv_event_t *e)
{
    /* Contact bounce on this panel double-fires otherwise. */
    const uint32_t now = lv_tick_get();
    if (now - s_kp_last_ms < KP_DEBOUNCE_MS) {
        return;
    }
    s_kp_last_ms = now;

    const char *key = lv_event_get_user_data(e);
    if (key == NULL || key[0] == '\0') {
        return;
    }

    const int f = s_active;

    if (strcmp(key, LV_SYMBOL_BACKSPACE) == 0) {
        s_values[f] /= 10;
        if (s_typed > 0) {
            s_typed--;
        }
        refresh_field(f);
        return;
    }

    if (key[0] < '0' || key[0] > '9') {
        return;
    }

    const int digit = key[0] - '0';
    s_values[f] = s_typed == 0 ? digit : s_values[f] * 10 + digit;
    s_typed++;
    refresh_field(f);

    /* Field full: clamp it and move on, so a date is four taps and not
     * four taps plus three selections. */
    if (s_typed >= s_form->digits[f]) {
        clamp_field(f);
        refresh_field(f);

        if (f + 1 < s_form->count) {
            highlight(f + 1);
        }
    }
}

static bool values_are_valid(void)
{
    for (int i = 0; i < s_form->count; i++) {
        if (s_values[i] < s_form->min[i] || s_values[i] > s_form->max[i]) {
            return false;
        }
    }

    return true;
}

/* Each editor changes its own half and keeps the other, so setting the
 * date does not blank the time and vice versa. */
static void commit(void)
{
    if (!values_are_valid()) {
        ESP_LOGW(TAG, "out of range, not committing");
        return;
    }

    struct tm now;
    link_time_get_local(&now);

    if (s_form == &FORM_DATE) {
        now.tm_mday = s_values[0];
        now.tm_mon = s_values[1] - 1;
        now.tm_year = s_values[2] - 1900;
    } else {
        now.tm_hour = s_values[0];
        now.tm_min = s_values[1];
        now.tm_sec = 0;
    }

    if (link_time_set(&now) != ESP_OK) {
        ESP_LOGE(TAG, "RTC write failed");
    }
}

static void close_cb(void *arg)
{
    (void)arg;

    if (s_overlay != NULL) {
        lv_obj_delete(s_overlay);
        s_overlay = NULL;
    }

    const bool committed = s_pending_commit;
    s_pending_commit = false;

    if (committed) {
        commit();
    }

    /* Taken and cleared before the call: the callback may open another
     * editor, which installs callbacks of its own. */
    const ui_time_edit_cb_t on_commit = s_on_commit;
    const ui_time_edit_cb_t on_cancel = s_on_cancel;
    s_on_commit = NULL;
    s_on_cancel = NULL;

    if (committed && on_commit != NULL) {
        on_commit();
    } else if (!committed && on_cancel != NULL) {
        on_cancel();
    }
}

static void on_back(void *user_data)
{
    (void)user_data;
    s_pending_commit = false;
    lv_async_call(close_cb, NULL);
}

static void on_validate(void *user_data)
{
    (void)user_data;
    s_pending_commit = true;
    lv_async_call(close_cb, NULL);
}

static void build_fields(lv_obj_t *parent, int32_t y)
{
    int32_t total = 0;
    for (int i = 0; i < s_form->count; i++) {
        total += s_form->width[i];
    }
    total += (s_form->count - 1) * SEP_W;

    int32_t x = LINK_USABLE_X + (LINK_USABLE_W - total) / 2;

    for (int i = 0; i < s_form->count; i++) {
        lv_obj_t *btn = ui_touch_make_tap_button(parent, on_field_tapped,
                                                 (void *)(intptr_t)i);
        lv_obj_remove_style_all(btn);
        lv_obj_set_size(btn, s_form->width[i], FIELD_H);
        lv_obj_set_pos(btn, x, y);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(btn, LINK_RADIUS_BTN, 0);
        lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *lbl = lv_label_create(btn);
        lv_obj_set_style_text_font(lbl, LINK_FONT_TITLE, 0);
        lv_obj_center(lbl);

        s_field_btn[i] = btn;
        s_field_lbl[i] = lbl;
        refresh_field(i);

        x += s_form->width[i];

        if (i + 1 < s_form->count) {
            lv_obj_t *sep = lv_label_create(parent);
            lv_label_set_text(sep, s_form->separator);
            lv_obj_set_style_text_font(sep, LINK_FONT_TITLE, 0);
            lv_obj_set_style_text_color(sep, LINK_COLOR_TEXT_DIM, 0);
            lv_obj_set_width(sep, SEP_W);
            lv_obj_set_style_text_align(sep, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_pos(sep, x,
                           y + (FIELD_H - LINK_FONT_TITLE->line_height) / 2);
            x += SEP_W;
        }
    }
}

static void build_keypad(lv_obj_t *parent, int32_t y)
{
    const int32_t key_w = (LINK_USABLE_W - (KP_COLS - 1) * KP_GAP) / KP_COLS;

    for (int r = 0; r < KP_ROWS; r++) {
        for (int c = 0; c < KP_COLS; c++) {
            /* The grid has one blank slot; nothing is drawn there. */
            if (KP_LABELS[r][c][0] == '\0') {
                continue;
            }

            lv_obj_t *btn = lv_button_create(parent);
            lv_obj_remove_style_all(btn);
            lv_obj_add_style(btn, &link_style_btn, 0);
            lv_obj_add_style(btn, &link_style_btn_pressed, LV_STATE_PRESSED);
            lv_obj_set_size(btn, key_w, KP_KEY_H);
            lv_obj_set_pos(btn, LINK_USABLE_X + c * (key_w + KP_GAP),
                           y + r * (KP_KEY_H + KP_GAP));
            lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_event_cb(btn, on_keypad_key, LV_EVENT_CLICKED,
                                (void *)KP_LABELS[r][c]);

            lv_obj_t *lbl = lv_label_create(btn);
            lv_label_set_text(lbl, KP_LABELS[r][c]);
            lv_obj_set_style_text_font(lbl, LINK_FONT_TITLE, 0);
            lv_obj_center(lbl);
        }
    }
}

static void open_editor(const form_t *form, ui_time_edit_cb_t on_commit,
                        ui_time_edit_cb_t on_cancel)
{
    if (s_overlay != NULL) {
        return;
    }

    s_form = form;
    s_on_commit = on_commit;
    s_on_cancel = on_cancel;
    s_pending_commit = false;
    s_kp_last_ms = 0;

    /* Start from what the clock says, so a correction is a digit or two
     * rather than the whole value. */
    struct tm now;
    link_time_get_local(&now);

    if (form == &FORM_DATE) {
        s_values[0] = now.tm_mday;
        s_values[1] = now.tm_mon + 1;
        s_values[2] = now.tm_year + 1900;
    } else {
        s_values[0] = now.tm_hour;
        s_values[1] = now.tm_min;
    }

    s_overlay = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(s_overlay);
    lv_obj_set_size(s_overlay, LINK_SCREEN_W, LINK_SCREEN_H);
    lv_obj_set_pos(s_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_overlay, LINK_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);

    ui_helpers_attach_overlay_header(s_overlay, st(form->title), on_back, NULL,
                                     on_validate, NULL);

    const int32_t grid_h = KP_ROWS * KP_KEY_H + (KP_ROWS - 1) * KP_GAP;
    const int32_t grid_y = LINK_USABLE_Y + LINK_USABLE_H - grid_h;

    /* Fields centred in what is left between the header and the grid. */
    build_fields(s_overlay,
                 LINK_CONTENT_Y + (grid_y - LINK_CONTENT_Y - FIELD_H) / 2);
    build_keypad(s_overlay, grid_y);

    highlight(0);
}

void ui_screen_date_edit_open(ui_time_edit_cb_t on_commit,
                              ui_time_edit_cb_t on_cancel)
{
    open_editor(&FORM_DATE, on_commit, on_cancel);
}

void ui_screen_time_edit_open(ui_time_edit_cb_t on_commit,
                              ui_time_edit_cb_t on_cancel)
{
    open_editor(&FORM_TIME, on_commit, on_cancel);
}

bool ui_screen_time_edit_is_open(void)
{
    return s_overlay != NULL;
}
