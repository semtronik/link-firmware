/**
 * @file app_ui.h
 * @brief What the application must provide for the shared screens.
 *
 * The screens in this component are built by the application's own
 * ui_manager, which owns the screen table and the back stack. They call
 * back into it through the three functions below - declared here,
 * defined by whichever app is being built. An app that omits one does
 * not link.
 *
 * The app_ prefix marks that direction: everything else in this
 * component is provided to the application, these three are required
 * from it.
 */

#pragma once

#include <stdbool.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Standard header: title, optional back, optional validate.
 *
 * Without a back button the title sits left, as a root screen; with one
 * it is centred.
 *
 * @return The title label, so the caller can update it.
 */
lv_obj_t *app_ui_attach_header(lv_obj_t *screen, const char *title,
                               bool show_back, bool show_validate,
                               void (*on_validate)(void *),
                               void *validate_user_data);

/** @brief Pop one level of navigation. At the root, stays put. */
void app_ui_navigate_back(void);

/**
 * @brief Destroy every screen, rebuild the styles, build them again.
 *
 * How a theme change takes effect: styles cannot be re-initialised under
 * live objects, so the screens go first.
 */
void app_ui_rebuild(void);

/**
 * @brief Product name, as the about screen shows it. e.g. "LINK-LR".
 *
 * Must stay valid for the life of the screen; a string literal is the
 * usual case.
 */
const char *app_ui_product_name(void);

#ifdef __cplusplus
}
#endif
