#include "ui_helpers.h"

#include <stdint.h>
#include <time.h>

#include "link_battery.h"
#include "link_power.h"
#include "link_time.h"

static void style_scrollbar(lv_obj_t *obj)
{
    lv_obj_set_style_bg_color(obj, LINK_COLOR_TEXT_DIM, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(obj, LV_OPA_50, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(obj, 2, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(obj, 4, LV_PART_SCROLLBAR);
    lv_obj_set_style_pad_right(obj, 2, LV_PART_SCROLLBAR);

    lv_obj_set_scroll_dir(obj, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_AUTO);
}

lv_obj_t *ui_helpers_create_list_container(lv_obj_t *parent)
{
    lv_obj_t *list = lv_obj_create(parent);
    lv_obj_remove_style_all(list);
    lv_obj_add_style(list, &link_style_list_container, 0);
    style_scrollbar(list);

    return list;
}

lv_obj_t *ui_helpers_create_flex_list(lv_obj_t *parent)
{
    lv_obj_t *list = lv_obj_create(parent);
    lv_obj_remove_style_all(list);
    lv_obj_add_style(list, &link_style_list_container, 0);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_right(list, LINK_BEZEL_MARGIN, 0);

    lv_obj_set_size(list, LINK_SCREEN_W, LINK_CONTENT_H);
    lv_obj_set_pos(list, 0, LINK_CONTENT_Y);

    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(list, LINK_LIST_GAP, 0);

    style_scrollbar(list);

    return list;
}

lv_obj_t *ui_helpers_add_list_row(lv_obj_t *flex_list,
                                  const ui_list_row_spec_t *spec,
                                  ui_touch_tap_cb_t on_tap,
                                  void *user_data,
                                  lv_obj_t **out_value_label)
{
    lv_obj_t *row = ui_helpers_create_list_row(flex_list, spec, on_tap,
                                               user_data, out_value_label);
    lv_obj_set_size(row, LINK_USABLE_W, LINK_LIST_ROW_H_MIN);

    return row;
}

lv_obj_t *ui_helpers_create_list_row(lv_obj_t *container,
                                     const ui_list_row_spec_t *spec,
                                     ui_touch_tap_cb_t on_tap,
                                     void *user_data,
                                     lv_obj_t **out_value_label)
{
    const bool interactive = spec->kind != UI_LIST_ROW_READ;

    lv_obj_t *row;
    if (interactive) {
        row = ui_touch_make_scroll_safe_button(container, on_tap, user_data);
    } else {
        row = lv_obj_create(container);
        lv_obj_remove_style_all(row);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
    }

    lv_obj_add_style(row, &link_style_list_row, 0);
    if (interactive) {
        lv_obj_add_style(row, &link_style_btn_pressed, LV_STATE_PRESSED);
    }
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    int32_t label_x = 0;

    if (spec->kind == UI_LIST_ROW_NAV && spec->icon != NULL) {
        lv_obj_t *icon = lv_label_create(row);
        lv_label_set_text(icon, spec->icon);
        lv_obj_set_style_text_color(icon, LINK_COLOR_ACCENT_FG, 0);
        lv_obj_set_style_text_font(icon, LINK_FONT_TITLE, 0);
        lv_obj_align(icon, LV_ALIGN_LEFT_MID, 0, 0);
        label_x = LINK_PAD_XL + LINK_PAD_LG;
    }

    /* The chevron says the row is actionable. READ rows do without: the
     * dimmed value already reads as "look, don't touch". */
    int32_t right_reserved = 0;
    if (interactive) {
        lv_obj_t *chevron = lv_label_create(row);
        lv_label_set_text(chevron, LV_SYMBOL_RIGHT);
        lv_obj_set_style_text_color(chevron, LINK_COLOR_TEXT_DIM, 0);
        lv_obj_set_style_text_font(chevron, LINK_FONT_TITLE, 0);
        lv_obj_align(chevron, LV_ALIGN_RIGHT_MID, 0, 0);
        right_reserved = LINK_PAD_XL + LINK_PAD_SM;
    }

    lv_obj_t *value_lbl = NULL;
    if (spec->kind != UI_LIST_ROW_NAV && spec->value != NULL) {
        value_lbl = lv_label_create(row);
        lv_label_set_text(value_lbl, spec->value);
        lv_obj_set_style_text_font(value_lbl, LINK_FONT_VALUE, 0);
        lv_obj_set_style_text_color(value_lbl,
                                    spec->kind == UI_LIST_ROW_READ
                                        ? LINK_COLOR_TEXT_DIM
                                        : LINK_COLOR_TEXT,
                                    0);
        lv_obj_align(value_lbl, LV_ALIGN_RIGHT_MID, -right_reserved, 0);
    }

    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, spec->label);
    lv_obj_set_style_text_font(lbl, LINK_FONT_BODY, 0);
    lv_obj_set_style_text_color(lbl, LINK_COLOR_TEXT, 0);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, label_x, 0);

    if (out_value_label != NULL) {
        *out_value_label = value_lbl;
    }

    return row;
}

lv_obj_t *ui_helpers_create_header_button(lv_obj_t *parent,
                                          const char *symbol,
                                          ui_touch_tap_cb_t on_tap,
                                          void *user_data)
{
    lv_obj_t *btn = ui_touch_make_tap_button(parent, on_tap, user_data);
    lv_obj_add_style(btn, &link_style_header_btn, 0);
    lv_obj_add_style(btn, &link_style_btn_pressed, LV_STATE_PRESSED);
    lv_obj_set_size(btn, LINK_HEADER_BTN_SZ, LINK_HEADER_BTN_SZ);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, symbol);
    lv_obj_center(lbl);

    return btn;
}

lv_obj_t *ui_helpers_attach_overlay_header(lv_obj_t *overlay,
                                           const char *title,
                                           ui_touch_tap_cb_t on_back,
                                           void *back_user_data,
                                           ui_touch_tap_cb_t on_validate,
                                           void *validate_user_data)
{
    lv_obj_t *bar = lv_obj_create(overlay);
    lv_obj_remove_style_all(bar);
    lv_obj_add_style(bar, &link_style_header, 0);
    lv_obj_set_size(bar, LINK_SCREEN_W, LINK_HEADER_H);
    lv_obj_set_pos(bar, 0, LINK_USABLE_Y);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    const int32_t btn_y = LINK_USABLE_Y + (LINK_HEADER_H - LINK_HEADER_BTN_SZ) / 2;

    /* The buttons go on the overlay rather than on the bar, so the bar
     * cannot swallow their touch area. */
    if (on_back != NULL) {
        lv_obj_t *back = ui_helpers_create_header_button(
            overlay, LV_SYMBOL_LEFT, on_back, back_user_data);
        lv_obj_set_pos(back, LINK_USABLE_X, btn_y);
    }

    lv_obj_t *lbl = lv_label_create(bar);
    lv_label_set_text(lbl, title);
    lv_obj_set_style_text_font(lbl, LINK_FONT_TITLE, 0);
    lv_obj_set_style_text_color(lbl, LINK_COLOR_TEXT, 0);
    lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);

    if (on_validate != NULL) {
        lv_obj_t *validate = ui_helpers_create_header_button(
            overlay, LV_SYMBOL_OK, on_validate, validate_user_data);
        lv_obj_set_pos(validate,
                       LINK_SCREEN_W - LINK_BEZEL_MARGIN - LINK_HEADER_BTN_SZ, btn_y);
        lv_obj_set_style_text_color(lv_obj_get_child(validate, 0),
                                    LINK_COLOR_ACCENT_FG, 0);
    }

    return lbl;
}

/*
 * Monitor status bar
 */
/*
 * The battery is read at a glance and from further away than the time
 * beside it, so it gets the next size up rather than the body font the
 * rest of the block uses. M28 was too much - the glyph alone would be
 * most of the header's height.
 */
#define STATUS_BATT_FONT   LINK_FONT_TITLE
#define STATUS_BATT_ICON_W 24

/*
 * One row: the title after the menu button, the dots centred, the clock
 * on the right. Everything is centred vertically in the band.
 */

/*
 * The title is centred on the screen, not placed after the menu button:
 * the pips sit centred under it and the two have to line up.
 *
 * Its width is what clears both the button and the clock on either
 * side, so a long name is ellipsised rather than running under them.
 */
#define STATUS_TITLE_EDGE \
    (LINK_USABLE_X + LINK_HEADER_BTN_SZ + LINK_PAD_SM)
/*
 * The centre column is a two-row grid, both rows the height of the
 * title's font box:
 *
 *   10  air
 *   18  row 1 - the title
 *   18  row 2 - the page indicator, centred in it
 *   10  air
 *
 * Symmetric by construction, and row 2 will hold whatever else the
 * header is asked to carry later without the spacing being retuned.
 * The clock and the menu button are not part of it: they are centred on
 * the whole band, each on its own.
 */
#define STATUS_ROW_H  18
#define STATUS_ROW1_Y ((LINK_HEADER_H - 2 * STATUS_ROW_H) / 2)
#define STATUS_ROW2_Y (STATUS_ROW1_Y + STATUS_ROW_H)



#define STATUS_TITLE_Y STATUS_ROW1_Y

/*
 * Page indicator: one stretched pill for where you are, round dots for
 * the pages either side.
 *
 * The pill is the part you are seeing, the dots are what is waiting, and
 * the shape itself says there is more than one. The accent is what makes
 * it register at 8 px - in grey it reads as decoration.
 */
#define STATUS_PILL_H   6
#define STATUS_PIP_H    STATUS_PILL_H
#define STATUS_DOT_W    STATUS_PILL_H
#define STATUS_PILL_W   27
#define STATUS_PIP_GAP  7

/*
 * The right-hand block: the widest of its two lines - "22/09/26" at
 * LINK_FONT_CAPTION, about 50 px - plus the battery icon and the gap
 * between them.
 *
 * Only used to bound the title beside it, so it is rounded up rather
 * than measured: erring wide costs the title a few pixels it does not
 * need, erring narrow lets a long name run under the date.
 */
#define STATUS_CLOCK_W 82

/*
 * Centred, so the width is twice the smaller half - whichever side is
 * more crowded sets it. The clock is wider than the menu button, so in
 * practice the clock does.
 */
#define STATUS_TITLE_RIGHT \
    (LINK_SCREEN_W - LINK_BEZEL_MARGIN - STATUS_CLOCK_W - LINK_PAD_MD)
#define STATUS_TITLE_W                                          \
    (2                                                          \
     * ((LINK_SCREEN_W / 2 - STATUS_TITLE_EDGE)                 \
                < (STATUS_TITLE_RIGHT - LINK_SCREEN_W / 2)      \
            ? (LINK_SCREEN_W / 2 - STATUS_TITLE_EDGE)           \
            : (STATUS_TITLE_RIGHT - LINK_SCREEN_W / 2)))

void ui_helpers_create_status_bar(lv_obj_t *screen,
                                  const char *title,
                                  ui_touch_tap_cb_t on_menu,
                                  void *menu_user_data,
                                  ui_status_bar_handles_t *out_handles)
{
    lv_obj_t *bar = lv_obj_create(screen);
    lv_obj_remove_style_all(bar);
    lv_obj_add_style(bar, &link_style_status_bar, 0);
    lv_obj_set_size(bar, LINK_SCREEN_W, LINK_HEADER_H);
    lv_obj_set_pos(bar, 0, LINK_USABLE_Y);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    /* The style's horizontal padding would fight the absolute
     * positioning below. */
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_add_flag(bar, LV_OBJ_FLAG_GESTURE_BUBBLE);

    /* On the screen rather than in the bar, so the bar cannot swallow
     * its touch area. Same x as a back button elsewhere, so nothing
     * shifts as the user moves between screens. */
    /* Secondary, like every other way out: the style's own TEXT_DIM, not
     * the accent the validate button takes. */
    lv_obj_t *menu = ui_helpers_create_header_button(screen, LV_SYMBOL_LIST,
                                                     on_menu, menu_user_data);
    lv_obj_set_pos(menu, LINK_USABLE_X,
                   LINK_USABLE_Y + (LINK_HEADER_H - LINK_SIGNATURE_BTN_SZ) / 2);
    lv_obj_add_flag(menu, LV_OBJ_FLAG_GESTURE_BUBBLE);

    /* The screen's name, brighter than the clock beside it: it says
     * where you are, which outranks what time it is. Bounded and
     * ellipsised so a long one is cut rather than run into the clock. */
    lv_obj_t *lbl_title = NULL;

    if (title != NULL) {
        lbl_title = lv_label_create(bar);
        lv_label_set_text(lbl_title, title);
        lv_obj_set_style_text_font(lbl_title, LINK_FONT_TITLE, 0);
        lv_obj_set_style_text_color(lbl_title, LINK_COLOR_TEXT, 0);
        lv_obj_set_style_text_align(lbl_title, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(lbl_title, STATUS_TITLE_W);
        lv_label_set_long_mode(lbl_title, LV_LABEL_LONG_DOT);


        /* Centred in the band. A screen with pips lifts it onto row 1 to
         * make room for them - see ui_helpers_status_bar_add_dots(). */
        lv_obj_align(lbl_title, LV_ALIGN_CENTER, 0, 0);
        lv_obj_add_flag(lbl_title, LV_OBJ_FLAG_GESTURE_BUBBLE);

    }

    /*
     * Date over time, with the battery beside them.
     *
     * A row of [ dates stacked | battery ]: the two text lines land on
     * the header's own two rows, and the icon is centred on the pair
     * rather than on either line - it belongs to both.
     *
     * Auto-sized and anchored by its RIGHT edge: Montserrat's digits
     * are proportional, so the text changes width as the time ticks.
     * Pinned left it would walk by a few pixels every minute; pinned
     * right the variation happens on the side facing the title, where
     * there is space and nothing to push.
     */
    lv_obj_t *center = lv_obj_create(bar);
    lv_obj_remove_style_all(center);
    lv_obj_set_size(center, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(center, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(center, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(center, LINK_PAD_SM, 0);

    lv_obj_align(center, LV_ALIGN_RIGHT_MID, -LINK_BEZEL_MARGIN, 0);
    lv_obj_remove_flag(center, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(center, LV_OBJ_FLAG_GESTURE_BUBBLE);

    /* The two lines, right-aligned on each other so the date and the
     * time share an edge whatever their widths. */
    lv_obj_t *stamp = lv_obj_create(center);
    lv_obj_remove_style_all(stamp);
    lv_obj_set_size(stamp, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(stamp, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(stamp, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_row(stamp, 0, 0);
    lv_obj_remove_flag(stamp, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(stamp, LV_OBJ_FLAG_GESTURE_BUBBLE);

    /* Smaller than the time below it: the date changes once a day. */
    lv_obj_t *lbl_date = lv_label_create(stamp);
    lv_label_set_text(lbl_date, "--/--/--");
    lv_obj_set_style_text_font(lbl_date, LINK_FONT_CAPTION, 0);
    lv_obj_set_style_text_color(lbl_date, LINK_COLOR_TEXT_DIM, 0);

    lv_obj_t *lbl_time = lv_label_create(stamp);
    lv_label_set_text(lbl_time, "--:--");
    lv_obj_set_style_text_font(lbl_time, LINK_FONT_BODY, 0);
    lv_obj_set_style_text_color(lbl_time, LINK_COLOR_TEXT_DIM, 0);

    /* Fixed-width wrapper so the bolt can sit on top of the battery
     * icon without either depending on the other's text width. */
    lv_obj_t *batt_wrap = lv_obj_create(center);
    lv_obj_remove_style_all(batt_wrap);
    lv_obj_set_size(batt_wrap, STATUS_BATT_ICON_W,
                    STATUS_BATT_FONT->line_height);
    lv_obj_remove_flag(batt_wrap, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(batt_wrap, LV_OBJ_FLAG_GESTURE_BUBBLE);

    lv_obj_t *icon_battery = lv_label_create(batt_wrap);
    lv_label_set_text(icon_battery, LV_SYMBOL_BATTERY_FULL);
    lv_obj_set_style_text_font(icon_battery, STATUS_BATT_FONT, 0);
    lv_obj_set_style_text_color(icon_battery, LINK_COLOR_TEXT_DIM, 0);
    lv_obj_set_style_text_align(icon_battery, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(icon_battery, STATUS_BATT_ICON_W);
    lv_obj_set_pos(icon_battery, 0, 0);

    /* Over the gauge, not instead of it: the level still matters while
     * charging. Plain white - the accent is reserved for the signature
     * button. */
    /* Left at body size while the gauge grew: the bolt has to read as a
     * mark ON the battery, and at the same size it competes with it. */
    lv_obj_t *icon_charging = lv_label_create(batt_wrap);
    lv_label_set_text(icon_charging, LV_SYMBOL_CHARGE);
    lv_obj_set_style_text_font(icon_charging, LINK_FONT_BODY, 0);
    lv_obj_set_style_text_color(icon_charging, LINK_COLOR_TEXT, 0);
    lv_obj_set_style_text_align(icon_charging, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(icon_charging, STATUS_BATT_ICON_W);
    /* Centred rather than pinned at 0,0: the wrapper is the battery's
     * height now, and a smaller bolt at the top would sit off it. */
    lv_obj_center(icon_charging);
    lv_obj_add_flag(icon_charging, LV_OBJ_FLAG_HIDDEN);

    if (out_handles != NULL) {
        out_handles->bar = bar;
        out_handles->lbl_title = lbl_title;
        out_handles->lbl_date = lbl_date;
        out_handles->lbl_time = lbl_time;
        out_handles->icon_battery = icon_battery;
        out_handles->icon_charging = icon_charging;
        out_handles->btn_menu = menu;

        for (int i = 0; i < UI_STATUS_DOTS_MAX; i++) {
            out_handles->dots[i] = NULL;
        }
        out_handles->dot_count = 0;
    }
}

void ui_helpers_status_bar_add_dots(ui_status_bar_handles_t *handles,
                                    int count,
                                    int initial_index)
{
    if (handles == NULL || count <= 0 || count > UI_STATUS_DOTS_MAX) {
        return;
    }


    lv_obj_t *bar = handles->bar;
    if (bar == NULL) {
        return;
    }

    /* The title sits centred in the band until pips need the room under
     * it; then it moves up onto row 1 and the two read as one block. */
    if (handles->lbl_title != NULL) {
        lv_obj_align(handles->lbl_title, LV_ALIGN_TOP_MID, 0,
                     STATUS_TITLE_Y);
    }

    const int32_t total_w = STATUS_PILL_W + (count - 1) * STATUS_DOT_W
                            + (count - 1) * STATUS_PIP_GAP;
    const int32_t start_x = (LINK_SCREEN_W - total_w) / 2;
    /*
     * Centred in what is left under the title's box, not inside a row of
     * its own.
     *
     * A second 18 px row held 4 px of pip and 14 px of nothing, and
     * centring in it put them 10 px under the title with 13 below -
     * high, and closer still under a title with a descender. Measuring
     * from the box's bottom edge instead is independent of which glyphs
     * the title happens to carry.
     */
    const int32_t y =
        STATUS_ROW2_Y + (LINK_HEADER_H - STATUS_ROW2_Y - STATUS_PIP_H) / 2;

    for (int i = 0; i < count; i++) {
        lv_obj_t *pip = lv_obj_create(bar);
        lv_obj_remove_style_all(pip);
        lv_obj_set_size(pip, STATUS_DOT_W, STATUS_PIP_H);
        lv_obj_set_style_radius(pip, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(pip, LV_OPA_COVER, 0);
        lv_obj_set_pos(pip, 0, y);
        lv_obj_remove_flag(pip, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(pip, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(pip, LV_OBJ_FLAG_GESTURE_BUBBLE);

        handles->dots[i] = pip;
    }

    handles->dot_count = count;
    handles->pip_start_x = start_x;

    ui_helpers_status_bar_set_page(handles, initial_index);
}

void ui_helpers_status_bar_set_page(const ui_status_bar_handles_t *handles,
                                    int index)
{
    if (handles == NULL || handles->dot_count <= 0) {
        return;
    }

    if (index < 0) {
        index = 0;
    }
    if (index >= handles->dot_count) {
        index = handles->dot_count - 1;
    }

    /* Widths change, so every pip is repositioned from the left each
     * time rather than nudged: the active one is a pill and the rest are
     * dots, and which is which has just moved. */
    int32_t x = handles->pip_start_x;

    for (int i = 0; i < handles->dot_count; i++) {
        lv_obj_t *pip = handles->dots[i];
        if (pip == NULL) {
            continue;
        }

        const bool active = (i == index);
        const int32_t w = active ? STATUS_PILL_W : STATUS_DOT_W;

        lv_obj_set_width(pip, w);
        lv_obj_set_x(pip, x);
        lv_obj_set_style_bg_color(
            pip, active ? LINK_COLOR_ACCENT_FG : LINK_COLOR_TEXT_DIM, 0);
        lv_obj_set_style_bg_opa(pip, active ? LV_OPA_COVER : LINK_OPA_INERT, 0);

        x += w + STATUS_PIP_GAP;
    }
}

/*
 * Pager
 *
 *   pager                        transparent, the bottom band
 *   |- target[i]  44x44          invisible, takes the tap
 *      |- dot[i]  8x8            what is actually seen
 *
 * The targets are taller than the band they sit in and overflow it
 * upwards on purpose: an 8 px dot is not a touch target.
 */
#define PAGER_MAX 4

typedef struct {
    ui_pager_change_cb_t cb;
    void *user_data;
    int index;
    lv_obj_t *pager;
} pager_slot_t;

static void pager_apply_styles(lv_obj_t *pager, int active, uint32_t count)
{
    for (uint32_t i = 0; i < count; i++) {
        lv_obj_t *target = lv_obj_get_child(pager, (int32_t)i);
        if (target == NULL) {
            continue;
        }

        lv_obj_t *dot = lv_obj_get_child(target, 0);
        if (dot == NULL) {
            continue;
        }

        lv_obj_remove_style(dot, &link_style_pager_dot_active, 0);
        lv_obj_remove_style(dot, &link_style_pager_dot_inactive, 0);
        lv_obj_add_style(dot,
                         (int)i == active ? &link_style_pager_dot_active
                                          : &link_style_pager_dot_inactive,
                         0);
    }
}

static void pager_tap_cb(void *user_data)
{
    pager_slot_t *slot = user_data;

    pager_apply_styles(slot->pager, slot->index,
                       lv_obj_get_child_count(slot->pager));

    if (slot->cb != NULL) {
        slot->cb(slot->index, slot->user_data);
    }
}

lv_obj_t *ui_helpers_create_pager_dots(lv_obj_t *parent,
                                       int count,
                                       int initial_index,
                                       ui_pager_change_cb_t on_change,
                                       void *user_data)
{
    if (count <= 0 || count > PAGER_MAX) {
        return NULL;
    }

    lv_obj_t *pager = lv_obj_create(parent);
    lv_obj_remove_style_all(pager);
    lv_obj_set_size(pager, LINK_USABLE_W, LINK_PAGER_H);
    lv_obj_set_pos(pager, LINK_USABLE_X,
                   LINK_USABLE_Y + LINK_USABLE_H - LINK_PAGER_H);
    lv_obj_set_style_bg_opa(pager, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(pager, 0, 0);
    lv_obj_set_style_border_width(pager, 0, 0);
    lv_obj_remove_flag(pager, LV_OBJ_FLAG_SCROLLABLE);

    /* Without this the targets are clipped to the band and the part
     * that overflows stops taking taps. */
    lv_obj_add_flag(pager, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_add_flag(pager, LV_OBJ_FLAG_GESTURE_BUBBLE);

    const int32_t stride = LINK_PAGER_DOT_SIZE + LINK_PAGER_DOT_GAP;
    const int32_t total_w =
        count * LINK_PAGER_DOT_SIZE + (count - 1) * LINK_PAGER_DOT_GAP;
    const int32_t start_x = (LINK_USABLE_W - total_w) / 2;

    static pager_slot_t s_slots[PAGER_MAX];

    for (int i = 0; i < count; i++) {
        s_slots[i].cb = on_change;
        s_slots[i].user_data = user_data;
        s_slots[i].index = i;
        s_slots[i].pager = pager;

        lv_obj_t *target =
            ui_touch_make_tap_button(pager, pager_tap_cb, &s_slots[i]);
        lv_obj_set_size(target, LINK_PAGER_TOUCH_TGT, LINK_PAGER_TOUCH_TGT);

        const int32_t dot_center_x =
            start_x + i * stride + LINK_PAGER_DOT_SIZE / 2;
        lv_obj_set_pos(target, dot_center_x - LINK_PAGER_TOUCH_TGT / 2,
                       (LINK_PAGER_H - LINK_PAGER_TOUCH_TGT) / 2);

        lv_obj_set_style_bg_opa(target, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(target, 0, 0);
        lv_obj_set_style_pad_all(target, 0, 0);
        lv_obj_remove_flag(target, LV_OBJ_FLAG_SCROLLABLE);

        /* A swipe that starts on a dot still belongs to the page. */
        lv_obj_add_flag(target, LV_OBJ_FLAG_GESTURE_BUBBLE);

        lv_obj_t *dot = lv_obj_create(target);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, LINK_PAGER_DOT_SIZE, LINK_PAGER_DOT_SIZE);
        lv_obj_center(dot);
        lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
    }

    pager_apply_styles(pager, initial_index, (uint32_t)count);

    return pager;
}

void ui_helpers_pager_set_active(lv_obj_t *pager, int index)
{
    if (pager == NULL) {
        return;
    }

    const uint32_t count = lv_obj_get_child_count(pager);
    if (count == 0) {
        return;
    }

    pager_apply_styles(pager, index, count);
}

/*
 * Signal block
 *
 * Thresholds and ranges match the desktop app, so a reading means the
 * same thing on both.
 */
#define SIGNAL_BAR_W   80
#define SIGNAL_BAR_H   4
#define SIGNAL_BLOCK_W (LINK_USABLE_W / 2)

lv_color_t ui_helpers_signal_color(ui_signal_type_t type, float v)
{
    if (type == UI_SIGNAL_RSSI) {
        if (v > -80.0f) {
            return LINK_COLOR_SUCCESS;
        }
        return v > -100.0f ? LINK_COLOR_WARNING : LINK_COLOR_ERROR;
    }

    if (v > 5.0f) {
        return LINK_COLOR_SUCCESS;
    }
    return v > 0.0f ? LINK_COLOR_WARNING : LINK_COLOR_ERROR;
}

static int signal_percent(ui_signal_type_t type, float v)
{
    const float min = type == UI_SIGNAL_RSSI ? -130.0f : -10.0f;
    const float max = type == UI_SIGNAL_RSSI ? -30.0f : 15.0f;

    float pct = (v - min) / (max - min) * 100.0f;

    if (pct < 0.0f) {
        pct = 0.0f;
    }
    if (pct > 100.0f) {
        pct = 100.0f;
    }

    return (int)(pct + 0.5f);
}

void ui_helpers_create_signal_value(lv_obj_t *parent,
                                    const char *caption,
                                    const char *unit,
                                    ui_signal_type_t type,
                                    ui_signal_block_t *out_block)
{
    /* Transparent: the values sit on the page, not in a card. */
    lv_obj_t *block = lv_obj_create(parent);
    lv_obj_remove_style_all(block);
    lv_obj_set_size(block, SIGNAL_BLOCK_W, LINK_MONITOR_CONTENT_H);
    lv_obj_set_style_bg_opa(block, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(block, 0, 0);
    lv_obj_set_style_pad_all(block, 0, 0);
    lv_obj_remove_flag(block, LV_OBJ_FLAG_SCROLLABLE);

    /* The swipe between views starts here as often as anywhere. */
    lv_obj_add_flag(block, LV_OBJ_FLAG_GESTURE_BUBBLE);

    lv_obj_t *lbl_caption = lv_label_create(block);
    lv_label_set_text(lbl_caption, caption);
    lv_obj_set_style_text_font(lbl_caption, LINK_FONT_CAPTION, 0);
    lv_obj_set_style_text_color(lbl_caption, LINK_COLOR_TEXT_DIM, 0);
    lv_obj_set_style_text_letter_space(lbl_caption, 2, 0);
    lv_obj_align(lbl_caption, LV_ALIGN_TOP_MID, 0, LINK_PAD_LG);

    lv_obj_t *lbl_value = lv_label_create(block);
    lv_label_set_text(lbl_value, "--");
    lv_obj_set_style_text_font(lbl_value, LINK_FONT_HUGE, 0);
    lv_obj_set_style_text_color(lbl_value, LINK_COLOR_TEXT_DIM, 0);
    lv_obj_align(lbl_value, LV_ALIGN_CENTER, -LINK_PAD_LG, 0);

    lv_obj_t *lbl_unit = lv_label_create(block);
    lv_label_set_text(lbl_unit, unit);
    lv_obj_set_style_text_font(lbl_unit, LINK_FONT_CAPTION, 0);
    lv_obj_set_style_text_color(lbl_unit, LINK_COLOR_TEXT_DIM, 0);
    lv_obj_align_to(lbl_unit, lbl_value, LV_ALIGN_OUT_RIGHT_BOTTOM, LINK_PAD_SM,
                    -LINK_PAD_XS);

    lv_obj_t *bar_bg = lv_obj_create(block);
    lv_obj_remove_style_all(bar_bg);
    lv_obj_set_size(bar_bg, SIGNAL_BAR_W, SIGNAL_BAR_H);
    lv_obj_set_style_bg_color(bar_bg, LINK_COLOR_SURFACE_ELEVATED, 0);
    lv_obj_set_style_bg_opa(bar_bg, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bar_bg, 2, 0);
    lv_obj_set_style_border_width(bar_bg, 0, 0);
    lv_obj_align(bar_bg, LV_ALIGN_BOTTOM_MID, 0, -LINK_PAD_LG);
    lv_obj_remove_flag(bar_bg, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(bar_bg, LV_OBJ_FLAG_GESTURE_BUBBLE);

    lv_obj_t *bar_fill = lv_obj_create(bar_bg);
    lv_obj_remove_style_all(bar_fill);
    lv_obj_set_size(bar_fill, 0, SIGNAL_BAR_H);
    lv_obj_set_pos(bar_fill, 0, 0);
    lv_obj_set_style_bg_color(bar_fill, LINK_COLOR_TEXT_DIM, 0);
    lv_obj_set_style_bg_opa(bar_fill, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bar_fill, 2, 0);

    if (out_block != NULL) {
        out_block->block = block;
        out_block->lbl_value = lbl_value;
        out_block->lbl_unit = lbl_unit;
        out_block->bar_bg = bar_bg;
        out_block->bar_fill = bar_fill;
        out_block->lbl_caption = lbl_caption;
        out_block->type = type;
    }
}

void ui_helpers_update_signal_value(ui_signal_block_t *block, float value,
                                    bool has_value)
{
    if (block == NULL) {
        return;
    }

    if (!has_value) {
        lv_label_set_text(block->lbl_value, "--");
        lv_obj_set_style_text_color(block->lbl_value, LINK_COLOR_TEXT_DIM, 0);
        lv_obj_set_style_text_color(block->lbl_unit, LINK_COLOR_TEXT_DIM, 0);
        lv_obj_set_size(block->bar_fill, 0, SIGNAL_BAR_H);
        return;
    }

    /* RSSI is whole dBm; SNR carries a decimal, where a tenth is the
     * difference between decoding and not. The tenth is formatted by
     * hand rather than with %f: this runs on the LVGL task, and the C
     * library's float conversion wants more stack than that task has. */
    if (block->type == UI_SIGNAL_RSSI) {
        lv_label_set_text_fmt(block->lbl_value, "%d",
                              (int)(value + (value >= 0 ? 0.5f : -0.5f)));
    } else {
        const int tenths = (int)(value * 10.0f + (value >= 0 ? 0.5f : -0.5f));
        const int whole = tenths / 10;
        const int frac = tenths < 0 ? -(tenths % 10) : tenths % 10;

        /* -0.4 has a whole part of 0, so the sign has to be carried
         * separately or it disappears. */
        lv_label_set_text_fmt(block->lbl_value, "%s%d.%d",
                              tenths < 0 && whole == 0 ? "-" : "", whole, frac);
    }

    const lv_color_t color = ui_helpers_signal_color(block->type, value);
    lv_obj_set_style_text_color(block->lbl_value, color, 0);
    lv_obj_set_style_text_color(block->lbl_unit, color, 0);
    lv_obj_set_style_bg_color(block->bar_fill, color, 0);

    lv_obj_set_size(block->bar_fill,
                    (SIGNAL_BAR_W * signal_percent(block->type, value)) / 100,
                    SIGNAL_BAR_H);

    /* The value's width changed, so the unit has to follow it. */
    lv_obj_align_to(block->lbl_unit, block->lbl_value,
                    LV_ALIGN_OUT_RIGHT_BOTTOM, LINK_PAD_XS, -LINK_PAD_XS);
}

/*
 * Stepper
 */
#define STEPPER_DEBOUNCE_MS 200
#define STEPPER_BTN_W       70
#define STEPPER_BTN_H       50

static lv_obj_t *s_stepper_overlay;
static lv_obj_t *s_stepper_lbl;
static int s_stepper_value;
static ui_stepper_cfg_t s_stepper_cfg;
static uint32_t s_stepper_last_ms;

static void stepper_refresh_label(void)
{
    char buf[24];

    if (s_stepper_cfg.format_cb != NULL) {
        s_stepper_cfg.format_cb(s_stepper_value, buf, sizeof(buf));
    } else {
        lv_snprintf(buf, sizeof(buf), s_stepper_cfg.format, s_stepper_value);
    }

    lv_label_set_text(s_stepper_lbl, buf);
}

static void stepper_step_cb(lv_event_t *e)
{
    const uint32_t now = lv_tick_get();
    if (now - s_stepper_last_ms < STEPPER_DEBOUNCE_MS) {
        return;
    }
    s_stepper_last_ms = now;

    const int sign = (int)(intptr_t)lv_event_get_user_data(e);
    int v = s_stepper_value + sign * s_stepper_cfg.step;

    if (v < s_stepper_cfg.min) {
        v = s_stepper_cfg.min;
    }
    if (v > s_stepper_cfg.max) {
        v = s_stepper_cfg.max;
    }
    s_stepper_value = v;

    stepper_refresh_label();

    if (s_stepper_cfg.on_preview != NULL) {
        s_stepper_cfg.on_preview(v);
    }
}

/* Deferred: the callback may build a screen, and doing that from inside
 * the event that is deleting this one corrupts the display list. */
static void stepper_close(void *arg)
{
    const bool commit = (bool)(intptr_t)arg;

    if (s_stepper_overlay != NULL) {
        lv_obj_delete(s_stepper_overlay);
        s_stepper_overlay = NULL;
        s_stepper_lbl = NULL;
    }

    if (commit) {
        if (s_stepper_cfg.on_commit != NULL) {
            s_stepper_cfg.on_commit(s_stepper_value);
        }
    } else if (s_stepper_cfg.on_cancel != NULL) {
        s_stepper_cfg.on_cancel();
    }
}

static void stepper_on_back(void *user_data)
{
    (void)user_data;
    lv_async_call(stepper_close, (void *)(intptr_t) false);
}

static void stepper_on_validate(void *user_data)
{
    (void)user_data;
    lv_async_call(stepper_close, (void *)(intptr_t) true);
}

/* Plain LVGL buttons, not ui_touch ones: these repeat on a long press,
 * which the tap guards would reject. */
static lv_obj_t *stepper_button(const char *text, int32_t x, int32_t y,
                                int sign)
{
    lv_obj_t *btn = lv_button_create(s_stepper_overlay);
    lv_obj_remove_style_all(btn);
    lv_obj_add_style(btn, &link_style_btn, 0);
    lv_obj_add_style(btn, &link_style_btn_pressed, LV_STATE_PRESSED);
    lv_obj_set_size(btn, STEPPER_BTN_W, STEPPER_BTN_H);
    lv_obj_set_pos(btn, x, y);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_add_event_cb(btn, stepper_step_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)sign);
    lv_obj_add_event_cb(btn, stepper_step_cb, LV_EVENT_LONG_PRESSED_REPEAT,
                        (void *)(intptr_t)sign);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, LINK_FONT_TITLE, 0);
    lv_obj_center(lbl);

    return btn;
}

void ui_stepper_open(const ui_stepper_cfg_t *cfg)
{
    if (s_stepper_overlay != NULL) {
        return;
    }

    s_stepper_cfg = *cfg;
    s_stepper_value = cfg->initial;
    s_stepper_last_ms = 0;

    s_stepper_overlay = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(s_stepper_overlay);
    lv_obj_set_size(s_stepper_overlay, LINK_SCREEN_W, LINK_SCREEN_H);
    lv_obj_set_pos(s_stepper_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_stepper_overlay, LINK_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_stepper_overlay, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_stepper_overlay, LV_OBJ_FLAG_SCROLLABLE);

    ui_helpers_attach_overlay_header(s_stepper_overlay, cfg->title,
                                     stepper_on_back, NULL,
                                     stepper_on_validate, NULL);

    const int32_t row_y = LINK_CONTENT_Y + (LINK_CONTENT_H - STEPPER_BTN_H) / 2;

    stepper_button("-", LINK_USABLE_X, row_y, -1);
    stepper_button("+", LINK_USABLE_X + LINK_USABLE_W - STEPPER_BTN_W, row_y, +1);

    s_stepper_lbl = lv_label_create(s_stepper_overlay);
    lv_obj_set_style_text_font(s_stepper_lbl, LINK_FONT_TITLE, 0);
    lv_obj_set_style_text_color(s_stepper_lbl, LINK_COLOR_TEXT, 0);
    lv_obj_set_width(s_stepper_lbl, LINK_USABLE_W - 2 * STEPPER_BTN_W);
    lv_obj_set_style_text_align(s_stepper_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_stepper_lbl, LINK_USABLE_X + STEPPER_BTN_W,
                   row_y + (STEPPER_BTN_H - LINK_FONT_TITLE->line_height) / 2);

    stepper_refresh_label();
}

bool ui_stepper_is_open(void)
{
    return s_stepper_overlay != NULL;
}

/*
 * List picker
 */
#define PICKER_MAX_OPTIONS 8
#define PICKER_RADIO_SZ    20

static lv_obj_t *s_picker_overlay;
static lv_obj_t *s_picker_dots[PICKER_MAX_OPTIONS];
static int s_picker_index;
static int s_picker_count;
static ui_list_picker_cfg_t s_picker_cfg;

static void picker_set_active(int index)
{
    s_picker_index = index;

    for (int i = 0; i < s_picker_count; i++) {
        if (s_picker_dots[i] == NULL) {
            continue;
        }

        lv_obj_remove_style(s_picker_dots[i], &link_style_radio_active, 0);
        lv_obj_remove_style(s_picker_dots[i], &link_style_radio_inactive, 0);
        lv_obj_add_style(s_picker_dots[i],
                         i == index ? &link_style_radio_active
                                    : &link_style_radio_inactive,
                         0);
    }
}

static void picker_on_row_tapped(void *user_data)
{
    picker_set_active((int)(intptr_t)user_data);
}

static void picker_close(void *arg)
{
    const bool commit = (bool)(intptr_t)arg;

    if (s_picker_overlay != NULL) {
        lv_obj_delete(s_picker_overlay);
        s_picker_overlay = NULL;
    }

    if (commit) {
        if (s_picker_cfg.on_commit != NULL) {
            s_picker_cfg.on_commit(s_picker_index);
        }
    } else if (s_picker_cfg.on_cancel != NULL) {
        s_picker_cfg.on_cancel();
    }
}

static void picker_on_back(void *user_data)
{
    (void)user_data;
    lv_async_call(picker_close, (void *)(intptr_t) false);
}

static void picker_on_validate(void *user_data)
{
    (void)user_data;
    lv_async_call(picker_close, (void *)(intptr_t) true);
}

void ui_list_picker_open(const ui_list_picker_cfg_t *cfg)
{
    if (s_picker_overlay != NULL) {
        return;
    }

    s_picker_cfg = *cfg;
    s_picker_count = cfg->count > PICKER_MAX_OPTIONS ? PICKER_MAX_OPTIONS
                                                     : cfg->count;

    for (int i = 0; i < PICKER_MAX_OPTIONS; i++) {
        s_picker_dots[i] = NULL;
    }

    s_picker_overlay = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(s_picker_overlay);
    lv_obj_set_size(s_picker_overlay, LINK_SCREEN_W, LINK_SCREEN_H);
    lv_obj_set_pos(s_picker_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_picker_overlay, LINK_COLOR_SURFACE, 0);
    lv_obj_set_style_bg_opa(s_picker_overlay, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_picker_overlay, LV_OBJ_FLAG_SCROLLABLE);

    ui_helpers_attach_overlay_header(s_picker_overlay, cfg->title,
                                     picker_on_back, NULL, picker_on_validate,
                                     NULL);

    lv_obj_t *list = ui_helpers_create_list_container(s_picker_overlay);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_size(list, LINK_SCREEN_W, LINK_CONTENT_H);
    lv_obj_set_pos(list, 0, LINK_CONTENT_Y);

    for (int i = 0; i < s_picker_count; i++) {
        lv_obj_t *row = ui_touch_make_tap_button(list, picker_on_row_tapped,
                                                 (void *)(intptr_t)i);
        lv_obj_add_style(row, &link_style_list_row, 0);
        lv_obj_add_style(row, &link_style_btn_pressed, LV_STATE_PRESSED);
        lv_obj_set_size(row, LINK_USABLE_W, LINK_LIST_ROW_H_MIN);
        lv_obj_set_pos(row, 0, i * (LINK_LIST_ROW_H_MIN + LINK_LIST_GAP));
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *dot = lv_obj_create(row);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, PICKER_RADIO_SZ, PICKER_RADIO_SZ);
        lv_obj_align(dot, LV_ALIGN_LEFT_MID, 0, 0);
        s_picker_dots[i] = dot;

        lv_obj_t *lbl = lv_label_create(row);
        lv_label_set_text(lbl, cfg->labels[i]);
        lv_obj_set_style_text_font(lbl, LINK_FONT_BODY, 0);
        lv_obj_set_style_text_color(lbl, LINK_COLOR_TEXT, 0);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID,
                     PICKER_RADIO_SZ + LINK_PAD_XL, 0);
    }

    picker_set_active(cfg->initial);
}

bool ui_list_picker_is_open(void)
{
    return s_picker_overlay != NULL;
}

void ui_helpers_update_status_bar(const ui_status_bar_handles_t *handles)
{
    /* Date over time, on the header's two rows. No seconds: their
     * ticking pulls the eye off the content, and the frame rows carry
     * them where they matter. */
    struct tm now;
    link_time_get_local(&now);

    if (link_time_is_valid()) {
        lv_label_set_text_fmt(handles->lbl_time, "%02d:%02d", now.tm_hour,
                              now.tm_min);
        if (handles->lbl_date != NULL) {
            lv_label_set_text_fmt(handles->lbl_date, "%02d/%02d/%02d",
                                  now.tm_mday, now.tm_mon + 1,
                                  now.tm_year % 100);
        }
    } else {
        lv_label_set_text(handles->lbl_time, "--:--");
        if (handles->lbl_date != NULL) {
            lv_label_set_text(handles->lbl_date, "--/--/--");
        }
    }

    link_power_t power;
    link_power_get(&power);

    const link_battery_t bat = power.battery;
    const bool on_battery =
        bat.presence == LINK_BATTERY_PRESENT && bat.gauge_ok;
    const unsigned pct = bat.soc_pct;

    /* The plug only stands in for a cell once one is known to be
     * missing. Before that the gauge's reading is shown: the detector
     * needs several seconds to reach a verdict, and a plug flashing up at
     * every boot would say something untrue. */
    const char *sym;
    if (bat.presence == LINK_BATTERY_ABSENT) {
        sym = LV_SYMBOL_USB;
    } else if (!bat.gauge_ok) {
        sym = LV_SYMBOL_BATTERY_EMPTY;
    } else if (pct >= 80) {
        sym = LV_SYMBOL_BATTERY_FULL;
    } else if (pct >= 60) {
        sym = LV_SYMBOL_BATTERY_3;
    } else if (pct >= 40) {
        sym = LV_SYMBOL_BATTERY_2;
    } else if (pct >= 20) {
        sym = LV_SYMBOL_BATTERY_1;
    } else {
        sym = LV_SYMBOL_BATTERY_EMPTY;
    }

    lv_label_set_text(handles->icon_battery, sym);

    /*
     * Green, amber, red on the level itself, charging or not: what the
     * gauge reads does not change because a charger is plugged in, and
     * the bolt over the icon already says which way it is going.
     *
     * Only a real reading is coloured. With no cell, or before the
     * gauge has a verdict, the icon stays dim - a colour there would
     * grade something nobody measured.
     */
    lv_color_t col = LINK_COLOR_TEXT_DIM;
    if (on_battery) {
        if (pct >= 50) {
            col = LINK_COLOR_SUCCESS;
        } else if (pct >= 20) {
            col = LINK_COLOR_WARNING;
        } else {
            col = LINK_COLOR_ERROR;
        }
    }
    lv_obj_set_style_text_color(handles->icon_battery, col, 0);

    /* From the charger's status pin, not from the gauge's rate: the rate
     * is a filtered estimate that wanders around zero on a full cell,
     * which would blink the bolt on and off. */
    if (power.state == LINK_POWER_CHARGING) {
        lv_obj_remove_flag(handles->icon_charging, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(handles->icon_charging, LV_OBJ_FLAG_HIDDEN);
    }
}
