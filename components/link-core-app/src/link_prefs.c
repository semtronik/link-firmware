#include "link_prefs.h"

#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "link_prefs";

/* Shared by every app on the platform, so a unit keeps its look across a
 * reflash. */
#define NVS_NAMESPACE     "link"
#define NVS_KEY_THEME     "theme"
#define NVS_KEY_BACKLIGHT "backlight"
#define NVS_KEY_LANG      "lang"

#define DEFAULT_THEME_LIGHT false
#define DEFAULT_BACKLIGHT   100
#define DEFAULT_LANG        LINK_LANG_FR

static bool s_theme_light = DEFAULT_THEME_LIGHT;
static uint8_t s_backlight = DEFAULT_BACKLIGHT;
static link_lang_t s_lang = DEFAULT_LANG;

static const char *const LANG_NAMES[LINK_LANG_COUNT] = {
    [LINK_LANG_FR] = "Français",
    [LINK_LANG_EN] = "English",
};

static esp_err_t store_u8(const char *key, uint8_t value)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_u8(handle, key, value);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "could not store %s: %s", key, esp_err_to_name(err));
    }

    return err;
}

esp_err_t link_prefs_init(void)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        ESP_LOGI(TAG, "no stored preferences, using defaults");
        return ESP_OK;
    }

    uint8_t u8;
    if (nvs_get_u8(handle, NVS_KEY_THEME, &u8) == ESP_OK) {
        s_theme_light = u8 != 0;
    }
    if (nvs_get_u8(handle, NVS_KEY_BACKLIGHT, &u8) == ESP_OK) {
        s_backlight = u8;
    }
    if (nvs_get_u8(handle, NVS_KEY_LANG, &u8) == ESP_OK
        && u8 < LINK_LANG_COUNT) {
        s_lang = (link_lang_t)u8;
    }

    nvs_close(handle);

    ESP_LOGI(TAG, "theme %s, backlight %u%%, language %u",
             s_theme_light ? "light" : "dark", (unsigned)s_backlight,
             (unsigned)s_lang);

    return ESP_OK;
}

bool link_prefs_get_theme_light(void)
{
    return s_theme_light;
}

esp_err_t link_prefs_set_theme_light(bool light)
{
    s_theme_light = light;

    return store_u8(NVS_KEY_THEME, light ? 1 : 0);
}

uint8_t link_prefs_get_backlight(void)
{
    return s_backlight;
}

esp_err_t link_prefs_set_backlight(uint8_t pct)
{
    s_backlight = pct;

    return store_u8(NVS_KEY_BACKLIGHT, pct);
}

link_lang_t link_prefs_get_lang(void)
{
    return s_lang;
}

esp_err_t link_prefs_set_lang(link_lang_t lang)
{
    if (lang >= LINK_LANG_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    s_lang = lang;

    return store_u8(NVS_KEY_LANG, (uint8_t)lang);
}

const char *link_prefs_lang_name(link_lang_t lang)
{
    return lang < LINK_LANG_COUNT ? LANG_NAMES[lang] : "?";
}
