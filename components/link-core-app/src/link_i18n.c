#include "link_i18n.h"

#include "esp_log.h"

static const char *TAG = "link_i18n";

const char *link_i18n_get(const link_i18n_row_t *table, size_t count, int id)
{
    if (id < 0 || (size_t)id >= count) {
        return "?";
    }

    const char *text = table[id][link_prefs_get_lang()];
    if (text == NULL) {
        text = table[id][LINK_LANG_FR];
    }

    return text != NULL ? text : "?";
}

void link_i18n_check(const link_i18n_row_t *table, size_t count,
                     const char *name)
{
    for (size_t id = 0; id < count; id++) {
        for (int lang = 0; lang < LINK_LANG_COUNT; lang++) {
            if (table[id][lang] == NULL) {
                ESP_LOGE(TAG, "%s: text %u has no language %d", name,
                         (unsigned)id, lang);
            }
        }
    }
}
