/*
 * The profile screen - the profile, and the settings that follow from it.
 *
 * Reached from the capture's session sheet, on the profile block.
 *
 *   +---------------------------------------+
 *   | <         Profil radio                |   the header
 *   +---------------------------------------+
 *   |          Meshtastic         v         |   the band
 *   +---------------------------------------+
 *   | FREQUENCE                    868 MHz  |
 *   | ETALEMENT                        SF6  |   the list
 *   | ...                                   |
 *
 * A BAND, NOT A ROW
 *
 * Nine layouts tried to say "the radio settings ARE the profile" inside a
 * list - indenting them under the name, boxing the two together, a pill
 * straddling the box, and finally the selector as the list's first row.
 * Every one of them was either a diagram to decode or, in the last case, a
 * profile filed among the six parameters it commands.
 *
 * So the profile leaves the list entirely. It sits in a band of its own
 * under the header, on its own fill, and everything below the separator
 * answers to it. The hierarchy is the position on the screen - there is
 * nothing left to interpret, and choosing another profile rewrites the rows
 * underneath in place, in front of the user.
 *
 * A DROPDOWN, NOT A SUB-SCREEN
 *
 * The band opens a list anchored under itself rather than the full-screen
 * picker. The picker has a header, a back button and a validate button: it
 * is somewhere one goes, and coming back from it is a second decision. The
 * dropdown hangs off the band it came from, leaves the header and the band
 * visible above it, and a tap anywhere else dismisses it. Choosing is one
 * tap, and so is changing one's mind.
 *
 * Each row carries the profile's name AND its settings, because three slots
 * called M1, M2, M3 say nothing about which is which. That is also why the
 * dropdown is not a stepper with arrows: stepping needs one tap per profile
 * and a "2 / 6" counter to make up for the list one cannot see.
 *
 * The name is centred in the band: it is a section title before it is a
 * control, and the chevron on the right is understood without sitting
 * against it.
 *
 * EVERY PROFILE IS EDITABLE
 *
 * Including the imported ones. An imported profile whose settings are
 * changed no longer matches the protocol it is named after - but refusing
 * the edit means the only way to try "Meshtastic but SF11" is to copy six
 * values by hand into a manual slot, and that is the experiment this tool
 * exists for. The name is a starting point, not a warranty.
 *
 * The id field is deliberately absent: an offset and a width are properties
 * of a protocol, and two numbers entered by hand produce an identifier that
 * looks right and means nothing. They belong to the parser JSON.
 *
 * The screen does not know how a value is entered: ui_param_edit picks the
 * list, the steppers or the keypad from the parameter itself. Rows read back
 * from the profile on every refresh, so what is shown is what the radio
 * actually took - a setting it refused never appears here.
 */

#include "esp_log.h"
#include "ui_helpers.h"
#include "ui_manager.h"
#include "ui_param_edit.h"
#include "link_profile.h"
#include "link_rec.h"
#include "link_theme.h"
#include "ui_text.h"

static const char *TAG = "ui_config";

/* One line of name, and air around it. */
#define BAND_H 40

/* Thicker and darker than the rows' own BORDER rules, or it reads as one
 * more of them. */
#define BAND_RULE_H 2
#define BAND_GAP    LINK_PAD_MD

#define BAND_LIST_Y (LINK_CONTENT_Y + BAND_H + BAND_RULE_H + BAND_GAP)

static lv_obj_t *s_band_name;
static lv_obj_t *s_screen;

static lv_obj_t *s_values[UI_PARAM_COUNT];

/*
 * The dropdown, while it is open. A row is the name AND its main settings -
 * "M1 - 868.5 SF11" - because the names alone do not identify them.
 */
static lv_obj_t *s_drop;
static char s_drop_rows[LINK_PROFILE_MAX][48];

static void on_row_tapped(void *user_data)
{
    ui_param_edit_open((ui_param_t)(uintptr_t)user_data);
}

static void drop_close(void *arg)
{
    (void)arg;

    if (s_drop != NULL) {
        lv_obj_delete(s_drop);
        s_drop = NULL;
    }
}

/*
 * Deleting the dropdown from inside a callback of one of its own children
 * would free the object LVGL is still dispatching an event to, so the
 * delete is deferred to the end of the frame. The profile is set here, and
 * the rows are rewritten straight away - the dropdown is still on screen
 * while that happens, and that is what shows the change taking effect.
 */
static void on_drop_pick(void *user_data)
{
    const int index = (int)(intptr_t)user_data;

    link_rec_close("profile");
    if (link_profile_set_current(index) != ESP_OK) {
        ESP_LOGW(TAG, "profile %d refused by the radio", index);
    }

    ui_screen_config_refresh();
    lv_async_call(drop_close, NULL);
}

/* A tap on the scrim: away from the list, so nothing is chosen. */
static void on_drop_dismiss(void *user_data)
{
    (void)user_data;
    lv_async_call(drop_close, NULL);
}

/*
 * Screens are built once and kept, so leaving this one with the dropdown
 * open would leave it hanging there on the way back.
 *
 * On the unload event and not in the refresh: the refresh runs on the UI
 * tick, several times a second, and a dropdown closed from there is shut
 * again a few hundred milliseconds after it opens - which reads on the
 * glass as a combo that will not stay open.
 */
static void on_screen_unloaded(lv_event_t *e)
{
    (void)e;
    drop_close(NULL);
}

static void on_band_tapped(void *user_data)
{
    (void)user_data;

    if (s_drop != NULL) {
        /* A second tap on the band closes it again. */
        lv_async_call(drop_close, NULL);
        return;
    }

    const int count = link_profile_count() < LINK_PROFILE_MAX
                          ? link_profile_count()
                          : LINK_PROFILE_MAX;

    /*
     * The catcher, so a tap anywhere outside the list dismisses. It is what
     * makes the dropdown feel like a menu hanging off the band rather than
     * another screen.
     *
     * It starts BELOW the band rather than at the top of the screen, so a
     * second tap on the band reaches the band and toggles the list shut,
     * which is what a combo does. The header stays live for the same
     * reason: leaving the screen with the list open is a legitimate move.
     */
    const int32_t scrim_y = LINK_CONTENT_Y + BAND_H;

    s_drop = ui_touch_make_tap_button(s_screen, on_drop_dismiss, NULL);
    lv_obj_remove_style_all(s_drop);
    lv_obj_set_size(s_drop, LINK_SCREEN_W, LINK_SCREEN_H - scrim_y);
    lv_obj_set_pos(s_drop, 0, scrim_y);
    lv_obj_set_style_bg_color(s_drop, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_drop, LV_OPA_30, 0);
    lv_obj_remove_flag(s_drop, LV_OBJ_FLAG_SCROLLABLE);

    /*
     * The list itself, hanging from the bottom edge of the band. It may run
     * over the parameters below - that is the point, it came from up there
     * and it goes back - but never over the band or the header, so its
     * origin stays in sight.
     */
    int32_t h = count * LINK_LIST_ROW_H_MIN + 2 * LINK_PAD_SM;

    if (scrim_y + h > LINK_SCREEN_H - LINK_PAD_SM) {
        h = LINK_SCREEN_H - LINK_PAD_SM - scrim_y;
    }

    /* Flush with the top of the scrim, which is the bottom of the band. */
    lv_obj_t *list = lv_obj_create(s_drop);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, LINK_USABLE_W, h);
    lv_obj_set_pos(list, LINK_USABLE_X, 0);
    lv_obj_set_style_bg_color(list, LINK_COLOR_SURFACE_ELEVATED, 0);
    lv_obj_set_style_bg_opa(list, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(list, LINK_RADIUS_CARD, 0);
    lv_obj_set_style_border_width(list, 1, 0);
    lv_obj_set_style_border_color(list, LINK_COLOR_BORDER, 0);
    lv_obj_set_style_border_opa(list, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(list, LINK_PAD_SM, 0);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);

    const int current = link_profile_current();

    for (int i = 0; i < count; i++) {
        link_profile_format_summary(i, s_drop_rows[i],
                                    sizeof(s_drop_rows[i]));

        lv_obj_t *row = ui_touch_make_tap_button(list, on_drop_pick,
                                                 (void *)(intptr_t)i);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, LINK_USABLE_W - 2 * LINK_PAD_SM,
                        LINK_LIST_ROW_H_MIN);
        lv_obj_set_pos(row, 0, i * LINK_LIST_ROW_H_MIN);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_radius(row, LINK_RADIUS_BTN, 0);
        lv_obj_add_style(row, &link_style_btn_pressed, LV_STATE_PRESSED);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *lbl = lv_label_create(row);
        lv_label_set_text(lbl, s_drop_rows[i]);
        lv_obj_set_style_text_font(lbl, LINK_FONT_BODY, 0);
        /* The one in force is in full strength, the rest dimmed: which is
         * current has to be readable without a radio button taking a
         * column from the names. */
        lv_obj_set_style_text_color(
            lbl, i == current ? LINK_COLOR_TEXT : LINK_COLOR_TEXT_DIM, 0);
        lv_obj_set_width(lbl, LINK_USABLE_W - 2 * LINK_PAD_SM - 24);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, LINK_PAD_SM, 0);

        if (i != current) {
            continue;
        }

        lv_obj_t *tick = lv_label_create(row);
        lv_label_set_text(tick, LV_SYMBOL_OK);
        lv_obj_set_style_text_font(tick, LINK_FONT_CAPTION, 0);
        lv_obj_set_style_text_color(tick, LINK_COLOR_ACCENT_FG, 0);
        lv_obj_align(tick, LV_ALIGN_RIGHT_MID, -LINK_PAD_SM, 0);
    }
}

/* The band and the hairline that closes it off from the list. */
static void build_band(lv_obj_t *screen)
{
    lv_obj_t *band = ui_touch_make_tap_button(screen, on_band_tapped, NULL);
    lv_obj_remove_style_all(band);
    lv_obj_set_size(band, LINK_SCREEN_W, BAND_H);
    lv_obj_set_pos(band, 0, LINK_CONTENT_Y);
    /* Its own fill, a shade off the screen's: what separates the band from
     * the list is that it is not part of it. */
    lv_obj_set_style_bg_color(band, LINK_COLOR_SURFACE_ELEVATED, 0);
    lv_obj_set_style_bg_opa(band, LV_OPA_COVER, 0);
    lv_obj_add_style(band, &link_style_btn_pressed, LV_STATE_PRESSED);
    lv_obj_remove_flag(band, LV_OBJ_FLAG_SCROLLABLE);

    /* Name and chevron are one string, so the arrow follows the name's
     * width instead of sitting out at the margin. */
    s_band_name = lv_label_create(band);
    lv_label_set_text(s_band_name, "");
    lv_obj_set_style_text_font(s_band_name, LINK_FONT_TITLE, 0);
    lv_obj_set_style_text_color(s_band_name, LINK_COLOR_ACCENT_FG, 0);
    lv_obj_set_style_text_align(s_band_name, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_band_name, LINK_SCREEN_W - 2 * LINK_HEADER_BTN_SZ);
    lv_label_set_long_mode(s_band_name, LV_LABEL_LONG_DOT);
    lv_obj_align(s_band_name, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t *line = lv_obj_create(screen);
    lv_obj_remove_style_all(line);
    lv_obj_set_size(line, LINK_SCREEN_W, BAND_RULE_H);
    lv_obj_set_pos(line, 0, LINK_CONTENT_Y + BAND_H);
    /* Heavier than the row borders: this is what detaches the band. */
    lv_obj_set_style_bg_color(line, LINK_COLOR_TEXT_DIM, 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
}

lv_obj_t *ui_screen_config_create(void)
{
    lv_obj_t *screen = lv_obj_create(NULL);
    lv_obj_remove_style_all(screen);
    lv_obj_add_style(screen, &link_style_screen, 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(screen, LINK_COLOR_SURFACE, 0);

    s_screen = screen;
    s_drop = NULL;

    lv_obj_add_event_cb(screen, on_screen_unloaded, LV_EVENT_SCREEN_UNLOADED,
                        NULL);

    /* Neutral, as every header is: the profile in force is in the band, not
     * in the title. */
    app_ui_attach_header(screen, tr(T_RADIO_PROFILE), true, false, NULL, NULL);

    build_band(screen);

    lv_obj_t *list = ui_helpers_create_flex_list(screen);
    /* The helper pins the list to the whole content area; here the band has
     * taken the top of it. */
    lv_obj_set_height(list, LINK_CONTENT_H - BAND_H - BAND_RULE_H - BAND_GAP);
    lv_obj_set_pos(list, 0, BAND_LIST_Y);

    for (int i = 0; i < UI_PARAM_COUNT; i++) {
        char value[24];
        ui_param_format((ui_param_t)i, value, sizeof(value));

        const ui_list_row_spec_t spec = {
            .kind = UI_LIST_ROW_EDIT,
            .label = ui_param_label((ui_param_t)i),
            .value = value,
        };
        ui_helpers_add_list_row(list, &spec, on_row_tapped,
                                (void *)(uintptr_t)i, &s_values[i]);
    }

    ESP_LOGD(TAG, "built");

    return screen;
}

void ui_screen_config_refresh(void)
{
    if (s_band_name == NULL) {
        return;
    }

    lv_label_set_text_fmt(s_band_name, "%s  " LV_SYMBOL_DOWN,
                          link_profile_name(link_profile_current()));

    for (int i = 0; i < UI_PARAM_COUNT; i++) {
        char value[24];
        ui_param_format((ui_param_t)i, value, sizeof(value));
        lv_label_set_text(s_values[i], value);
    }
}
