/**
 * @file ui_param_edit.h
 * @brief The one radio-parameter editor.
 *
 * A single full-screen template - back, title, validate - whose centre
 * is the only part that changes: a list of discrete values, a pair of
 * +/- buttons, or a keypad. Which one a parameter gets is decided here,
 * not by whoever opens the editor, so a screen never has to know how a
 * value is entered.
 *
 * Nothing is applied while editing. A local copy of the profile is
 * changed, and [OK] pushes the whole set through link_profile; [back]
 * drops it.
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* In the order the config screen lists them. */
typedef enum {
    UI_PARAM_REGION,
    UI_PARAM_FREQ,
    UI_PARAM_SF,
    UI_PARAM_BW,
    UI_PARAM_CR,
    UI_PARAM_SYNC,
    UI_PARAM_POWER,
    UI_PARAM_PREAMBLE,
    UI_PARAM_CRC,
    UI_PARAM_IQ,
    UI_PARAM_RX_GAIN,
    UI_PARAM_COUNT,
} ui_param_t;

/** @brief Name of a parameter, as the config screen labels it. */
const char *ui_param_label(ui_param_t param);

/**
 * @brief Format a parameter's current value into @p buf.
 *
 * Shared by the config rows and the editor, so a value reads the same
 * in both.
 */
void ui_param_format(ui_param_t param, char *buf, unsigned size);

/** @brief Open the editor. Idempotent while one is already open. */
void ui_param_edit_open(ui_param_t param);


/** @brief True while the editor is up. */
bool ui_param_edit_is_open(void);

#ifdef __cplusplus
}
#endif
