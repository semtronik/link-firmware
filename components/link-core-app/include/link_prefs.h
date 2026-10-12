/**
 * @file link_prefs.h
 * @brief Interface preferences, persisted.
 *
 * Theme, backlight, language: what the user set on the screens, kept across
 * boots. They belong to the platform rather than to any one application,
 * so every app reads and writes the same NVS namespace - a unit keeps
 * its look whichever firmware is flashed on it.
 *
 * Radio or bus settings do not belong here. They answer to a module and
 * live with it.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Interface languages, in the order every translation table follows. */
typedef enum {
    LINK_LANG_FR,
    LINK_LANG_EN,
    LINK_LANG_COUNT,
} link_lang_t;

/**
 * @brief Load the stored preferences, falling back to the defaults.
 *
 * Call after nvs_flash_init() and before the first screen is built. A
 * missing key keeps its default, so a fresh device and one that has only
 * ever had its radio touched behave the same.
 */
esp_err_t link_prefs_init(void);

/** @brief True when the light theme is selected. Dark by default. */
bool link_prefs_get_theme_light(void);

esp_err_t link_prefs_set_theme_light(bool light);

/** @brief Backlight, in percent. Stored as set - 5% is still readable. */
uint8_t link_prefs_get_backlight(void);

esp_err_t link_prefs_set_backlight(uint8_t pct);

/** @brief Interface language. French by default. */
link_lang_t link_prefs_get_lang(void);

esp_err_t link_prefs_set_lang(link_lang_t lang);

/** @brief A language's name in that language: "Français", "English". */
const char *link_prefs_lang_name(link_lang_t lang);

#ifdef __cplusplus
}
#endif
