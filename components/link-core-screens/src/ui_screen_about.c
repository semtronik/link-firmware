/*
 * About - static identity: part number, hardware serial, firmware
 * version. Read-only rows, no refresh.
 */

#include "screens_text.h"
#include "board_pins.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "link_screens.h"
#include "link_theme.h"
#include "ui_helpers.h"

static const char *TAG = "ui_about";

lv_obj_t *ui_screen_about_create(void)
{
    lv_obj_t *screen = lv_obj_create(NULL);
    lv_obj_remove_style_all(screen);
    lv_obj_add_style(screen, &link_style_screen, 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    /* The rows fill the content area in surface; the bezel strip below
     * them matches rather than showing a band of background. */
    lv_obj_set_style_bg_color(screen, LINK_COLOR_SURFACE, 0);

    app_ui_attach_header(screen, st(ST_ABOUT), true, false, NULL, NULL);

    const esp_app_desc_t *desc = esp_app_get_description();

    static char version[32];
    lv_snprintf(version, sizeof(version), "%s", desc ? desc->version : "?");

    /* The factory eFuse MAC is burned in at manufacture and never
     * changes, which makes it the board's serial number. */
    static char serial[20];
    uint8_t mac[6];
    if (esp_read_mac(mac, ESP_MAC_EFUSE_FACTORY) == ESP_OK) {
        lv_snprintf(serial, sizeof(serial), "%02X:%02X:%02X:%02X:%02X:%02X",
                    mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    } else {
        lv_snprintf(serial, sizeof(serial), "?");
    }

    const ui_list_row_spec_t rows[] = {
        {.kind = UI_LIST_ROW_READ, .label = "PN", .value = app_ui_product_name()},
        {.kind = UI_LIST_ROW_READ, .label = "MAC", .value = serial},
        {.kind = UI_LIST_ROW_READ, .label = "Firmware", .value = version},
        {.kind = UI_LIST_ROW_READ, .label = st(ST_BOARD), .value = BOARD_REV_STRING},
    };

    lv_obj_t *list = ui_helpers_create_flex_list(screen);

    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        ui_helpers_add_list_row(list, &rows[i], NULL, NULL, NULL);
    }

    ESP_LOGD(TAG, "built");

    return screen;
}
