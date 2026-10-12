#include "link_screens.h"

#include "link_theme.h"

lv_obj_t *ui_screen_splash_create(const char *version)
{
    lv_obj_t *screen = lv_obj_create(NULL);
    lv_obj_remove_style_all(screen);
    lv_obj_set_style_bg_color(screen, LINK_COLOR_ACCENT, 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    /* Dark on the accent, like the signature button. */
    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, "Link-LR");
    lv_obj_set_style_text_font(title, LINK_FONT_TITLE, 0);
    lv_obj_set_style_text_color(title, LINK_COLOR_TEXT_ON_ACCENT, 0);
    lv_obj_set_style_text_letter_space(title, 1, 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -LINK_PAD_LG);

    if (version != NULL) {
        lv_obj_t *ver = lv_label_create(screen);
        lv_label_set_text(ver, version);
        lv_obj_set_style_text_font(ver, LINK_FONT_BODY, 0);
        lv_obj_set_style_text_color(ver, LINK_COLOR_TEXT_ON_ACCENT, 0);
        lv_obj_set_style_text_opa(ver, LV_OPA_70, 0);
        lv_obj_align(ver, LV_ALIGN_CENTER, 0, LINK_PAD_LG);
    }

    return screen;
}
