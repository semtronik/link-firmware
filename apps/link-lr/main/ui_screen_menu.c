/*
 * Menu - what is not the measurement.
 *
 * The capture is the root, and everything it needs is on it. This holds
 * the rest: the device's own settings and the pages one reads once.
 *
 * Tiles rather than a list: these are unrelated destinations, not the
 * subdivisions of one thing.
 */

#include "esp_log.h"
#include "ui_helpers.h"
#include "ui_manager.h"
#include "link_theme.h"
#include "ui_text.h"

static const char *TAG = "ui_menu";

typedef struct {
    t_id_t title;
    const char *icon;
    ui_screen_id_t target;
} menu_tile_t;

/* Not a screen: the reset asks first, over whatever is showing. */
#define TARGET_FACTORY_RESET UI_SCREEN_COUNT

/* Titles are cut to the 112 px the text column leaves. The reset comes
 * last, alone on its row, away from the tiles tapped every day. */
static const menu_tile_t TILES[] = {
    {T_PREFERENCES, LV_SYMBOL_IMAGE, UI_SCREEN_SYSTEM},
    {T_DATE_TIME, LV_SYMBOL_BELL, UI_SCREEN_DATETIME},
    {T_SYSINFO, LV_SYMBOL_LIST, UI_SCREEN_SYSINFO},
    {T_ABOUT, LV_SYMBOL_FILE, UI_SCREEN_ABOUT},
    {T_FACTORY_RESET, LV_SYMBOL_WARNING, TARGET_FACTORY_RESET},
};

#define TILE_COUNT ((int)(sizeof(TILES) / sizeof(TILES[0])))

#define TILE_COLS 2
#define TILE_ROWS ((TILE_COUNT + TILE_COLS - 1) / TILE_COLS)

/*
 * LINK_CONTENT_Y stops at the bottom of the header and does not carry the
 * gap the layout grid puts under it.
 */
#define BAND_AIR 8
#define BAND_Y   (LINK_CONTENT_Y + BAND_AIR)
#define BAND_H   (LINK_CONTENT_H - BAND_AIR)

/* Every tile on one page: nothing to scroll. */
#define TILE_GAP 8
#define TILE_W   ((LINK_USABLE_W - TILE_GAP) / TILE_COLS)
#define TILE_H   ((BAND_H - (TILE_ROWS - 1) * TILE_GAP) / TILE_ROWS)

#define TILE_ICON_W 24
#define TILE_PAD    LINK_PAD_SM
#define TILE_ICON_X TILE_PAD

#define TILE_TEXT_X (TILE_ICON_X + TILE_ICON_W + LINK_PAD_SM)

static lv_obj_t *s_screen;

static void on_tile_tapped(void *user_data)
{
    const ui_screen_id_t target = (ui_screen_id_t)(uintptr_t)user_data;

    if (target == TARGET_FACTORY_RESET) {
        ui_screen_factory_reset_open();
        return;
    }
    ui_manager_switch_to(target);
}

static void build_tile(lv_obj_t *page, const menu_tile_t *tile, int slot)
{
    const int32_t x = (slot % TILE_COLS) * (TILE_W + TILE_GAP);
    const int32_t y = (slot / TILE_COLS) * (TILE_H + TILE_GAP);
    /* A tile alone on the last row takes the whole width. */
    const bool alone = slot == TILE_COUNT - 1 && slot % TILE_COLS == 0;
    const int32_t w = alone ? LINK_USABLE_W : TILE_W;
    const int32_t text_w = w - TILE_TEXT_X - TILE_PAD;

    lv_obj_t *btn = ui_touch_make_tap_button(page, on_tile_tapped,
                                             (void *)(uintptr_t)tile->target);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, w, TILE_H);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_style_bg_color(btn, LINK_COLOR_SURFACE, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, LINK_RADIUS_CARD, 0);

    /* The outline carries the tile, not the fill: it is what says "button"
     * in both themes. */
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, LINK_COLOR_BORDER, 0);
    lv_obj_set_style_border_opa(btn, LV_OPA_COVER, 0);

    lv_obj_add_style(btn, &link_style_btn_pressed, LV_STATE_PRESSED);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *icon = lv_label_create(btn);
    lv_label_set_text(icon, tile->icon);
    lv_obj_set_style_text_font(icon, LINK_FONT_TITLE, 0);
    lv_obj_set_style_text_color(icon, LINK_COLOR_ACCENT_FG, 0);
    lv_obj_set_style_text_align(icon, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(icon, TILE_ICON_W);
    lv_obj_align(icon, LV_ALIGN_LEFT_MID, TILE_ICON_X, 0);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, tr(tile->title));
    lv_obj_set_style_text_font(lbl, LINK_FONT_BODY, 0);
    lv_obj_set_style_text_color(lbl, LINK_COLOR_TEXT, 0);
    lv_obj_set_width(lbl, text_w);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, TILE_TEXT_X, 0);
}

lv_obj_t *ui_screen_menu_create(void)
{
    s_screen = lv_obj_create(NULL);
    lv_obj_add_style(s_screen, &link_style_screen, 0);
    lv_obj_remove_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);

    app_ui_attach_header(s_screen, tr(T_MENU), true, false, NULL, NULL);

    lv_obj_t *band = lv_obj_create(s_screen);
    lv_obj_remove_style_all(band);
    lv_obj_set_size(band, LINK_USABLE_W, BAND_H);
    lv_obj_set_pos(band, LINK_USABLE_X, BAND_Y);
    lv_obj_remove_flag(band, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < TILE_COUNT; i++) {
        build_tile(band, &TILES[i], i);
    }

    ESP_LOGD(TAG, "built: %d tiles", TILE_COUNT);

    return s_screen;
}

void ui_screen_menu_refresh(void)
{
}
