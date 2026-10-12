/*
 * Preferences - the settings that are not the radio's. Edit rows, each
 * opening the component that suits its type.
 */

#include "esp_log.h"
#include "link_prefs.h"
#include "link_display.h"
#include "screens_text.h"
#include "ui_helpers.h"
#include "link_screens.h"
#include "link_theme.h"

static const char *TAG = "ui_system";

static lv_obj_t *s_theme_value;
static lv_obj_t *s_backlight_value;
static lv_obj_t *s_lang_value;

/* Mirrors what link_prefs holds, so a cancelled edit can put the
 * backlight back where it was without a read. */
static uint8_t s_backlight;

/* Filled when a picker opens: the labels follow the language. */
static const char *s_theme_labels[2];
static const char *s_lang_labels[LINK_LANG_COUNT];

static const char *theme_name(bool light)
{
    return st(light ? ST_THEME_LIGHT : ST_THEME_DARK);
}

static void theme_on_commit(int index)
{
    const bool light = index == 0;

    if (light == link_theme_is_light()) {
        return;
    }

    link_theme_set_light(light);
    link_prefs_set_theme_light(light);

    /* Every screen goes and comes back: the styles cannot be rebuilt
     * under objects that still point at them. */
    app_ui_rebuild();
}

static void on_theme_tapped(void *user_data)
{
    (void)user_data;

    if (ui_list_picker_is_open()) {
        return;
    }

    s_theme_labels[0] = theme_name(true);
    s_theme_labels[1] = theme_name(false);

    const ui_list_picker_cfg_t cfg = {
        .title = st(ST_THEME),
        .labels = s_theme_labels,
        .count = 2,
        .initial = link_theme_is_light() ? 0 : 1,
        .on_commit = theme_on_commit,
    };
    ui_list_picker_open(&cfg);
}

static void lang_on_commit(int index)
{
    if ((link_lang_t)index == link_prefs_get_lang()) {
        return;
    }

    link_prefs_set_lang((link_lang_t)index);
    ESP_LOGI(TAG, "language %s", link_prefs_lang_name((link_lang_t)index));

    /* Texts are set when a screen is built: rebuilding is what changes
     * them all. */
    app_ui_rebuild();
}

static void on_lang_tapped(void *user_data)
{
    (void)user_data;

    if (ui_list_picker_is_open()) {
        return;
    }

    for (int i = 0; i < LINK_LANG_COUNT; i++) {
        s_lang_labels[i] = link_prefs_lang_name((link_lang_t)i);
    }

    const ui_list_picker_cfg_t cfg = {
        .title = st(ST_LANGUAGE),
        .labels = s_lang_labels,
        .count = LINK_LANG_COUNT,
        .initial = (int)link_prefs_get_lang(),
        .on_commit = lang_on_commit,
    };
    ui_list_picker_open(&cfg);
}

static void backlight_on_preview(int value)
{
    link_display_backlight_set((uint8_t)value);
}

static void backlight_on_commit(int value)
{
    s_backlight = (uint8_t)value;
    link_prefs_set_backlight(s_backlight);
    ESP_LOGI(TAG, "backlight %u%%", (unsigned)s_backlight);
}

static void backlight_on_cancel(void)
{
    link_display_backlight_set(s_backlight);
}

static void on_backlight_tapped(void *user_data)
{
    (void)user_data;

    if (ui_stepper_is_open()) {
        return;
    }

    const ui_stepper_cfg_t cfg = {
        .title = st(ST_BACKLIGHT),
        .format = "%d %%",
        .initial = s_backlight,
        /* Not down to zero: a dark screen looks like a dead board, and
         * the way back out is on that screen. */
        .min = 5,
        .max = 100,
        .step = 5,
        .on_commit = backlight_on_commit,
        .on_cancel = backlight_on_cancel,
        .on_preview = backlight_on_preview,
    };
    ui_stepper_open(&cfg);
}

lv_obj_t *ui_screen_system_create(void)
{
    s_backlight = link_prefs_get_backlight();

    lv_obj_t *screen = lv_obj_create(NULL);
    lv_obj_remove_style_all(screen);
    lv_obj_add_style(screen, &link_style_screen, 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(screen, LINK_COLOR_SURFACE, 0);

    app_ui_attach_header(screen, st(ST_PREFERENCES), true, false, NULL, NULL);

    lv_obj_t *list = ui_helpers_create_flex_list(screen);

    const ui_list_row_spec_t theme = {
        .kind = UI_LIST_ROW_EDIT,
        .label = st(ST_THEME),
        .value = theme_name(link_theme_is_light()),
    };
    ui_helpers_add_list_row(list, &theme, on_theme_tapped, NULL,
                            &s_theme_value);

    const ui_list_row_spec_t backlight = {
        .kind = UI_LIST_ROW_EDIT,
        .label = st(ST_BACKLIGHT),
        .value = "--",
    };
    ui_helpers_add_list_row(list, &backlight, on_backlight_tapped, NULL,
                            &s_backlight_value);

    const ui_list_row_spec_t lang = {
        .kind = UI_LIST_ROW_EDIT,
        .label = st(ST_LANGUAGE),
        .value = link_prefs_lang_name(link_prefs_get_lang()),
    };
    ui_helpers_add_list_row(list, &lang, on_lang_tapped, NULL, &s_lang_value);

    ESP_LOGD(TAG, "built");

    return screen;
}

void ui_screen_system_refresh(void)
{
    lv_label_set_text(s_theme_value, theme_name(link_theme_is_light()));
    lv_label_set_text_fmt(s_backlight_value, "%u %%", (unsigned)s_backlight);
    lv_label_set_text(s_lang_value, link_prefs_lang_name(link_prefs_get_lang()));
}
