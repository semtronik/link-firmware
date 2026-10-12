/**
 * @file main.c
 * @brief link-lr - LoRa P2P handheld.
 */

#include <stdio.h>

#include "board_core_bus.h"
#include "build_stamp.h"
#include "esp_app_desc.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "link_battery.h"
#include "link_profile.h"
#include "link_display.h"
#include "link_events.h"
#include "link_factory.h"
#include "link_lvgl.h"
#include "link_power.h"
#include "link_prefs.h"
#include "link_proto.h"
#include "link_proto_radio.h"
#include "link_proto_rec.h"
#include "link_radio.h"
#include "link_rec.h"
#include "link_regul.h"
#include "link_screens.h"
#include "link_screenshot.h"
#include "link_theme.h"
#include "link_time.h"
#include "link_touch.h"
#include "nvs_flash.h"
#include "power_key.h"
#include "ui_manager.h"
#include "ui_touch.h"

static const char *TAG = "link-lr";

/* Steps of the fade-in, and the pause between them. The panel is dark
 * until the first frame has been scanned out, so the ramp doubles as
 * the wait that keeps power-up GRAM noise off the glass. */
#define BACKLIGHT_STEP_PCT 5
#define BACKLIGHT_STEP_MS  10

/* Long enough to read the name, short enough not to be in the way. */
#define SPLASH_MS 1200

/* Ramps to whatever was stored rather than to full: coming up brighter
 * than the user left it and then dimming would be worse than starting
 * at the right level. */
static void backlight_fade_in(uint8_t target_pct)
{
    for (unsigned pct = 0; pct < target_pct; pct += BACKLIGHT_STEP_PCT) {
        ESP_ERROR_CHECK(link_display_backlight_set((uint8_t)pct));
        vTaskDelay(pdMS_TO_TICKS(BACKLIGHT_STEP_MS));
    }

    ESP_ERROR_CHECK(link_display_backlight_set(target_pct));
}

static void erase_recordings(void)
{
    link_rec_erase();
}

void app_main(void)
{
    /* First thing: hold the latch, before any delay. On battery this is
     * what takes over from the user's finger. */
    power_key_latch();

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_LOGI(TAG, "latch held %u ms after power-on", power_key_latch_delay_ms());

    /* A missing log must not stop the firmware: it is a diagnostic aid,
     * not a dependency. */
    if (link_events_init() != ESP_OK) {
        ESP_LOGW(TAG, "event log unavailable");
    }

    ESP_ERROR_CHECK(board_core_bus_init());
    ESP_ERROR_CHECK(link_battery_init());
    ESP_ERROR_CHECK(link_power_init());

    /* Before anything is drawn: the splash comes up in the stored theme
     * and at the stored brightness, rather than changing under the user
     * a moment later. */
    ESP_ERROR_CHECK(link_regul_init());
    ESP_ERROR_CHECK(link_profile_init());
    ESP_ERROR_CHECK(link_prefs_init());

    /* Before the radio: frames are stamped with this clock. */
    ESP_ERROR_CHECK(link_time_init());

    link_events_append("BOOT", NULL);

    ESP_ERROR_CHECK(link_display_panel_init());
    ESP_ERROR_CHECK(link_touch_init());

    /* LVGL takes the panel here: link_display_fill() stops working from
     * this point. */
    ESP_ERROR_CHECK(link_lvgl_init());

    const esp_app_desc_t *desc = esp_app_get_description();

    /* A dev build carries its timestamp on the splash, so two of them
     * can be told apart on the bench; a release shows the number alone.
     */
    /* esp_app_desc_t carries 32 bytes of version, and the stamp adds
     * "  build YYYYMMDD-HHMM" - sized so neither can be truncated. */
    char version[64] = "";
    if (desc != NULL) {
#if LINK_IS_RELEASE
        snprintf(version, sizeof(version), "%s", desc->version);
#else
        snprintf(version, sizeof(version), "%s  build %s", desc->version,
                 LINK_BUILD_STAMP);
#endif
    }

    link_lvgl_lock(UINT32_MAX);

    /* Before any screen exists: the thresholds belong to the input
     * devices, not to whichever screen happens to be built first. */
    ui_touch_tune_indevs();

    link_theme_set_light(link_prefs_get_theme_light());
    link_theme_init();
    lv_obj_t *splash = ui_screen_splash_create(version[0] ? version : NULL);
    lv_screen_load(splash);
    link_lvgl_unlock();

    /* Comes up on the splash rather than on a blank panel. */
    backlight_fade_in(link_prefs_get_backlight());

    power_key_start();

    /* Behind the splash, so the wait is doing something. A radio that
     * will not come up is not fatal: the screens say so, and everything
     * else still works. */
    link_profile_cfg_t profile;
    link_profile_get_current(&profile);

    if (link_radio_start(&profile.radio, profile.rx_boosted) != ESP_OK) {
        ESP_LOGE(TAG, "radio did not start - running without it");
        link_events_append("RADIO_ERROR", "no module");
    }

    if (link_rec_start() != ESP_OK) {
        ESP_LOGE(TAG, "recording did not start");
    }
    link_factory_set_app_erase(erase_recordings);

    /* Started before the screens so the app can set the clock while the
     * onboarding is still asking for it. */
    if (link_proto_radio_register() != ESP_OK) {
        ESP_LOGE(TAG, "radio protocol commands unavailable");
    }
    if (link_proto_rec_register() != ESP_OK) {
        ESP_LOGE(TAG, "recording protocol commands unavailable");
    }
    if (link_screenshot_register() != ESP_OK) {
        ESP_LOGE(TAG, "screenshot command unavailable");
    }

    if (link_proto_start("Link-LR") != ESP_OK) {
        ESP_LOGE(TAG, "serial protocol did not start");
    }

    vTaskDelay(pdMS_TO_TICKS(SPLASH_MS));

    /* Before the screens are built, so they come up in the language
     * chosen here. The clock blocks the monitor until it is set: a frame
     * stamped with a wrong time is worse than one not stamped at all. */
    const bool full = link_profile_needs_setup();
    if (full || !link_time_is_valid()) {
        ui_screen_setup_run(splash, full);
    }

    link_lvgl_lock(UINT32_MAX);
    ESP_ERROR_CHECK(ui_manager_init());
    lv_obj_delete(splash);
    link_lvgl_unlock();

    ESP_LOGI(TAG, "up - hold PWR_KEY %u ms to open the shutdown screen",
             POWER_KEY_HOLD_MS);
}
