/**
 * @file link_profile.h
 * @brief Profiles: every radio setting in one object, stored. The only
 *        place the settings live.
 *
 * Ten slots, in two kinds:
 *
 *   BASE     3 slots, always present. They start from a region's
 *            defaults - throughput, standard, range - and are edited from
 *            the config screen. Their name is "Profile N", composed, never
 *            stored: nothing on the device can rename them.
 *   IMPORTED 7 slots, filled by Link Studio, named by it. Editable too:
 *            the name is a starting point, not a warranty.
 *
 * Selecting or editing a profile APPLIES it: radio settings and gain go
 * to the chip together, the region to link_regul, and the slot to NVS
 * only once the chip has taken them - what is on air is what the screen
 * names.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "link_regul.h"
#include "sx1262_radio.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Base slots, first in the list and always present. */
#define LINK_PROFILE_BASE_COUNT 3

/** Imported slots, filled by Link Studio. */
#define LINK_PROFILE_IMPORTED_MAX 7

#define LINK_PROFILE_MAX \
    (LINK_PROFILE_BASE_COUNT + LINK_PROFILE_IMPORTED_MAX)

/** Longest profile name, terminator included. */
#define LINK_PROFILE_NAME_LEN 24

/** Room for link_profile_current_json(), whatever the name. */
#define LINK_PROFILE_JSON_MAX 384

typedef struct {
    sx1262_radio_cfg_t radio;
    bool rx_boosted;
    link_region_t region;
} link_profile_cfg_t;

/**
 * @brief Load the stored profiles and the selected one.
 *
 * Call after nvs_flash_init() and link_regul_init(), before
 * link_radio_start(), which takes link_profile_get_current(). Hands the
 * selected profile's region to link_regul. A device with nothing stored
 * comes up with the EU868 defaults and Profile 1 selected.
 */
esp_err_t link_profile_init(void);

/** @brief True when no base profile is stored: the first-start setup is due. */
bool link_profile_needs_setup(void);

/**
 * @brief Fill the base profiles with @p region's defaults, store them,
 *        and select and apply Profile 1.
 */
esp_err_t link_profile_setup(link_region_t region);

/** @brief How many profiles are on offer. Never 0. */
int link_profile_count(void);

/** @brief Name of a profile, as the screens show it. */
const char *link_profile_name(int index);

/** @brief Name of a profile as recordings carry it, whatever the language. */
const char *link_profile_record_name(int index);

/** @brief Which profile is selected. */
int link_profile_current(void);

/** @brief Settings of a profile. */
void link_profile_get(int index, link_profile_cfg_t *out);

/** @brief Settings of the selected profile. */
void link_profile_get_current(link_profile_cfg_t *out);

/**
 * @brief Select a profile and apply it.
 *
 * The chip refusing the settings leaves the selection unchanged.
 */
esp_err_t link_profile_set_current(int index);

/**
 * @brief Apply new settings to the selected profile, then store them in
 *        its slot.
 *
 * Nothing is stored if the chip refuses them.
 */
esp_err_t link_profile_update_current(const link_profile_cfg_t *cfg);

/**
 * @brief Add or replace an imported profile.
 *
 * For Link Studio, over the serial link. Replaces an entry of the same
 * name if there is one, so re-importing a corrected profile updates it
 * rather than filling a second slot.
 *
 * @return ESP_ERR_NO_MEM when the imported slots are full.
 */
esp_err_t link_profile_import(const char *name, const link_profile_cfg_t *cfg);

/** @brief True when no profile has been imported yet. */
bool link_profile_none_imported(void);

/**
 * @brief The selected profile as the JSON object recordings and the
 *        protocol carry: {"name":...,"region":...,...}.
 *
 * One writer for both, so they cannot drift apart.
 *
 * @return Its length, or 0 if @p size is too small.
 */
size_t link_profile_current_json(char *buf, size_t size);

/** @brief A bandwidth in kHz without its unit: "125", "62.5", "31.25". */
void link_profile_format_bw(uint32_t bw_hz, char *buf, unsigned size);

/**
 * @brief A profile's radio settings as one line: "868.5 MHz  SF9  125 kHz".
 *
 * Composed here rather than on each screen so the same profile reads
 * the same wherever it is shown.
 */
void link_profile_format_radio(int index, char *buf, unsigned size);

/**
 * @brief Name and settings on one line: "Profil 1 - 868.3 MHz SF7 BW125".
 *
 * For a list row, which holds one string per entry and has the width
 * for both. The frequency carries up to three decimals, because that is
 * what tells 869.5 from 869.525 - two channels a coarser figure would
 * merge into one entry.
 */
void link_profile_format_summary(int index, char *buf, unsigned size);

#ifdef __cplusplus
}
#endif
