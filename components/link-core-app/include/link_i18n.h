/**
 * @file link_i18n.h
 * @brief Interface texts in the language of link_prefs.
 *
 * Each component keeps its own table: an enum of ids, and one row per id
 * holding the text in every language, in link_lang_t order. A text is
 * asked for by id, so a misspelt one does not compile. A missing
 * translation falls back to French and is logged by link_i18n_check().
 *
 * Logs, protocol messages and technical names (SF, CRC, EU868) are not
 * translated.
 */

#pragma once

#include <stddef.h>

#include "link_prefs.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef const char *const link_i18n_row_t[LINK_LANG_COUNT];

/** @brief Text @p id of @p table, in the current language. */
const char *link_i18n_get(const link_i18n_row_t *table, size_t count, int id);

/** @brief Log every row of @p table that lacks a language. */
void link_i18n_check(const link_i18n_row_t *table, size_t count,
                     const char *name);

#ifdef __cplusplus
}
#endif
