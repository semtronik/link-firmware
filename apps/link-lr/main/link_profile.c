#include "link_profile.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "link_radio.h"
#include "nvs.h"
#include "ui_text.h"

static const char *TAG = "link_profile";

#define NVS_NAMESPACE     "link-lr"
#define NVS_KEY_BASE      "prof-man"
#define NVS_KEY_IMPORTED  "prof-imp"
#define NVS_KEY_IMP_COUNT "prof-impn"
#define NVS_KEY_CURRENT   "prof-cur"

/* An empty name is a base slot's: composed, so it follows the language. */
typedef struct {
    char name[LINK_PROFILE_NAME_LEN];
    link_profile_cfg_t cfg;
} profile_slot_t;

static const t_id_t BASE_NAMES[LINK_PROFILE_BASE_COUNT] = {
    T_PROFILE_1, T_PROFILE_2, T_PROFILE_3,
};

static const char *const BASE_RECORD_NAMES[LINK_PROFILE_BASE_COUNT] = {
    "Profile 1", "Profile 2", "Profile 3",
};

/* Throughput, standard, range: one channel per region, only the
 * spreading factor moves. */
typedef struct {
    uint32_t freq_hz;
    uint32_t bw_hz;
    uint8_t sf[LINK_PROFILE_BASE_COUNT];
} region_defaults_t;

static const region_defaults_t DEFAULTS[LINK_REGION_COUNT] = {
    [LINK_REGION_EU868] = {868300000U, 125000U, {7, 9, 12}},
    [LINK_REGION_US915] = {915000000U, 500000U, {8, 10, 12}},
};

#define DEFAULT_POWER_DBM 14
#define RANGE_SLOT        2

static profile_slot_t s_base[LINK_PROFILE_BASE_COUNT];
static profile_slot_t s_imported[LINK_PROFILE_IMPORTED_MAX];
static int s_imported_count;

static int s_current;

/* No base profiles stored: a first boot, or one after a factory reset. */
static bool s_needs_setup;

/* s_lock guards the slots and the selection: the screens, the protocol
 * and the recorder read them from three tasks. s_apply_lock keeps two
 * writers from interleaving, so the slot stored is the one on air; it is
 * held across the chip's reconfiguration, s_lock never is. */
static SemaphoreHandle_t s_lock;
static SemaphoreHandle_t s_apply_lock;

static void base_default(link_region_t region, int index,
                         link_profile_cfg_t *out)
{
    const region_defaults_t *d = &DEFAULTS[region];

    sx1262_radio_default_cfg(&out->radio);
    out->radio.freq_hz = d->freq_hz;
    out->radio.bw_hz = d->bw_hz;
    out->radio.sf = d->sf[index];
    out->radio.cr = 5;
    out->radio.power_dbm = DEFAULT_POWER_DBM;
    out->rx_boosted = index == RANGE_SLOT;
    out->region = region;
}

static void seed_base(link_region_t region)
{
    for (int i = 0; i < LINK_PROFILE_BASE_COUNT; i++) {
        s_base[i].name[0] = '\0';
        base_default(region, i, &s_base[i].cfg);
    }
}

static bool cfg_usable(const link_profile_cfg_t *cfg)
{
    return cfg->region < LINK_REGION_COUNT
           && sx1262_radio_check_cfg(&cfg->radio) == ESP_OK;
}

static profile_slot_t *slot_at(int index)
{
    if (index >= 0 && index < LINK_PROFILE_BASE_COUNT) {
        return &s_base[index];
    }

    const int imported = index - LINK_PROFILE_BASE_COUNT;
    if (imported >= 0 && imported < s_imported_count) {
        return &s_imported[imported];
    }

    return NULL;
}

/* ---------------------------------------------------------------- */
/* NVS                                                              */
/* ---------------------------------------------------------------- */

/*
 * Base and imported slots go as two blobs: they are written at different
 * times by different things. A size mismatch means the struct changed
 * under an old record: it is dropped, and the defaults stand.
 */
static void nvs_load(void)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        ESP_LOGI(TAG, "no stored profiles, using defaults");
        return;
    }

    static profile_slot_t base[LINK_PROFILE_BASE_COUNT];
    size_t len = sizeof(base);
    if (nvs_get_blob(handle, NVS_KEY_BASE, base, &len) == ESP_OK) {
        if (len == sizeof(base)) {
            s_needs_setup = false;
            for (int i = 0; i < LINK_PROFILE_BASE_COUNT; i++) {
                if (cfg_usable(&base[i].cfg)) {
                    s_base[i] = base[i];
                    s_base[i].name[LINK_PROFILE_NAME_LEN - 1] = '\0';
                } else {
                    ESP_LOGW(TAG, "base slot %d unusable, reset", i + 1);
                }
            }
        } else {
            ESP_LOGW(TAG, "stored base profiles have the wrong size");
        }
    }

    static profile_slot_t imported[LINK_PROFILE_IMPORTED_MAX];
    len = sizeof(imported);
    uint8_t count = 0;
    if (nvs_get_blob(handle, NVS_KEY_IMPORTED, imported, &len) == ESP_OK
        && len == sizeof(imported)
        && nvs_get_u8(handle, NVS_KEY_IMP_COUNT, &count) == ESP_OK
        && count <= LINK_PROFILE_IMPORTED_MAX) {
        for (int i = 0; i < count; i++) {
            if (imported[i].name[0] == '\0' || !cfg_usable(&imported[i].cfg)) {
                continue;
            }
            s_imported[s_imported_count] = imported[i];
            s_imported[s_imported_count].name[LINK_PROFILE_NAME_LEN - 1] = '\0';
            s_imported_count++;
        }
    }

    uint8_t current = 0;
    if (nvs_get_u8(handle, NVS_KEY_CURRENT, &current) == ESP_OK) {
        s_current = current;
    }

    nvs_close(handle);
}

static void nvs_store_blob(const char *key, const void *data, size_t size,
                           int imported_count)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed, %s not stored", key);
        return;
    }

    esp_err_t err = nvs_set_blob(handle, key, data, size);
    if (err == ESP_OK && imported_count >= 0) {
        err = nvs_set_u8(handle, NVS_KEY_IMP_COUNT, (uint8_t)imported_count);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "could not store %s: %s", key, esp_err_to_name(err));
    }
}

static void nvs_store_base(void)
{
    nvs_store_blob(NVS_KEY_BASE, s_base, sizeof(s_base), -1);
}

static void nvs_store_imported(void)
{
    nvs_store_blob(NVS_KEY_IMPORTED, s_imported, sizeof(s_imported),
                   s_imported_count);
}

static void nvs_store_slot(int index)
{
    if (index < LINK_PROFILE_BASE_COUNT) {
        nvs_store_base();
    } else {
        nvs_store_imported();
    }
}

static void nvs_store_current(void)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        return;
    }

    if (nvs_set_u8(handle, NVS_KEY_CURRENT, (uint8_t)s_current) == ESP_OK) {
        nvs_commit(handle);
    }
    nvs_close(handle);
}

/* ---------------------------------------------------------------- */

static esp_err_t apply(const link_profile_cfg_t *cfg)
{
    const esp_err_t err = link_radio_configure(&cfg->radio, cfg->rx_boosted);
    if (err == ESP_OK) {
        link_regul_set_region(cfg->region);
    }
    return err;
}

esp_err_t link_profile_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_apply_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL || s_apply_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    seed_base(LINK_REGION_EU868);
    s_imported_count = 0;
    s_current = 0;
    s_needs_setup = true;

    nvs_load();

    /* A stored index can point past what is loaded. */
    if (s_current >= link_profile_count()) {
        s_current = 0;
    }

    link_regul_set_region(slot_at(s_current)->cfg.region);

    ESP_LOGI(TAG, "%d profiles (%d imported), current %d",
             link_profile_count(), s_imported_count, s_current);

    return ESP_OK;
}

bool link_profile_needs_setup(void)
{
    return s_needs_setup;
}

esp_err_t link_profile_setup(link_region_t region)
{
    if (region >= LINK_REGION_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_apply_lock, portMAX_DELAY);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    seed_base(region);
    s_current = 0;
    nvs_store_base();
    const link_profile_cfg_t cfg = s_base[0].cfg;
    xSemaphoreGive(s_lock);

    nvs_store_current();
    s_needs_setup = false;

    /* Stored whatever the chip says: the slots are the region's defaults,
     * and a missing radio must not send the next boot back here. */
    const esp_err_t err = apply(&cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "radio refused the defaults: %s", esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "base profiles for %s", link_regul_region_name(region));

    xSemaphoreGive(s_apply_lock);
    return err;
}

int link_profile_count(void)
{
    return LINK_PROFILE_BASE_COUNT + s_imported_count;
}

const char *link_profile_name(int index)
{
    const profile_slot_t *slot = slot_at(index);
    if (slot == NULL) {
        return tr(BASE_NAMES[0]);
    }

    return slot->name[0] != '\0' ? slot->name : tr(BASE_NAMES[index]);
}

const char *link_profile_record_name(int index)
{
    const profile_slot_t *slot = slot_at(index);
    if (slot == NULL) {
        return BASE_RECORD_NAMES[0];
    }

    return slot->name[0] != '\0' ? slot->name : BASE_RECORD_NAMES[index];
}

int link_profile_current(void)
{
    return s_current;
}

void link_profile_get(int index, link_profile_cfg_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const profile_slot_t *slot = slot_at(index);
    *out = slot != NULL ? slot->cfg : s_base[0].cfg;
    xSemaphoreGive(s_lock);
}

void link_profile_get_current(link_profile_cfg_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = slot_at(s_current)->cfg;
    xSemaphoreGive(s_lock);
}

esp_err_t link_profile_set_current(int index)
{
    xSemaphoreTake(s_apply_lock, portMAX_DELAY);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    const profile_slot_t *slot = slot_at(index);
    const link_profile_cfg_t cfg = slot != NULL ? slot->cfg : s_base[0].cfg;
    xSemaphoreGive(s_lock);

    if (slot == NULL) {
        xSemaphoreGive(s_apply_lock);
        return ESP_ERR_INVALID_ARG;
    }

    const esp_err_t err = apply(&cfg);
    if (err == ESP_OK) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_current = index;
        xSemaphoreGive(s_lock);
        nvs_store_current();
        ESP_LOGI(TAG, "profile -> %s", link_profile_name(index));
    } else {
        ESP_LOGW(TAG, "radio refused %s: %s", link_profile_name(index),
                 esp_err_to_name(err));
    }

    xSemaphoreGive(s_apply_lock);
    return err;
}

esp_err_t link_profile_update_current(const link_profile_cfg_t *cfg)
{
    if (cfg == NULL || !cfg_usable(cfg)) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_apply_lock, portMAX_DELAY);

    const esp_err_t err = apply(cfg);
    if (err == ESP_OK) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        slot_at(s_current)->cfg = *cfg;
        nvs_store_slot(s_current);
        xSemaphoreGive(s_lock);
        ESP_LOGD(TAG, "stored %s", link_profile_name(s_current));
    }

    xSemaphoreGive(s_apply_lock);
    return err;
}

esp_err_t link_profile_import(const char *name, const link_profile_cfg_t *cfg)
{
    if (name == NULL || cfg == NULL || name[0] == '\0' || !cfg_usable(cfg)) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = ESP_OK;
    xSemaphoreTake(s_apply_lock, portMAX_DELAY);
    xSemaphoreTake(s_lock, portMAX_DELAY);

    int i = 0;
    while (i < s_imported_count && strcmp(s_imported[i].name, name) != 0) {
        i++;
    }

    if (i < s_imported_count) {
        s_imported[i].cfg = *cfg;
        nvs_store_imported();
        ESP_LOGI(TAG, "replaced %s", name);
    } else if (s_imported_count >= LINK_PROFILE_IMPORTED_MAX) {
        err = ESP_ERR_NO_MEM;
    } else {
        profile_slot_t *slot = &s_imported[s_imported_count];
        snprintf(slot->name, sizeof(slot->name), "%s", name);
        slot->cfg = *cfg;
        s_imported_count++;
        nvs_store_imported();
        ESP_LOGI(TAG, "imported %s", name);
    }

    xSemaphoreGive(s_lock);
    xSemaphoreGive(s_apply_lock);
    return err;
}

static void json_put(char *buf, size_t size, size_t *len, const char *fmt, ...)
{
    if (*len >= size) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(buf + *len, size - *len, fmt, ap);
    va_end(ap);
    if (n > 0) {
        *len += (size_t)n;
    }
}

size_t link_profile_current_json(char *buf, size_t size)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const link_profile_cfg_t profile = slot_at(s_current)->cfg;
    char name[LINK_PROFILE_NAME_LEN];
    snprintf(name, sizeof(name), "%s", link_profile_record_name(s_current));
    xSemaphoreGive(s_lock);

    const sx1262_radio_cfg_t *cfg = &profile.radio;
    size_t len = 0;

    json_put(buf, size, &len, "{\"name\":\"");
    for (const char *c = name; *c != '\0'; c++) {
        const unsigned char ch = (unsigned char)*c;
        if (ch == '"' || ch == '\\') {
            json_put(buf, size, &len, "\\%c", ch);
        } else if (ch < 0x20) {
            json_put(buf, size, &len, "\\u%04x", ch);
        } else {
            json_put(buf, size, &len, "%c", ch);
        }
    }
    json_put(buf, size, &len,
             "\",\"region\":\"%s\",\"freq_hz\":%lu,\"sf\":%u,\"bw_hz\":%lu,"
             "\"cr\":%u,\"power_dbm\":%d,\"preamble\":%u,\"crc_on\":%s,"
             "\"iq_inverted\":%s,\"sync_word\":%u,\"rx_gain\":\"%s\"}",
             link_regul_region_name(profile.region),
             (unsigned long)cfg->freq_hz, (unsigned)cfg->sf,
             (unsigned long)cfg->bw_hz, (unsigned)cfg->cr, (int)cfg->power_dbm,
             (unsigned)cfg->preamble_len, cfg->crc_on ? "true" : "false",
             cfg->iq_inverted ? "true" : "false", (unsigned)cfg->sync_word,
             profile.rx_boosted ? "boosted" : "saving");

    return len < size ? len : 0;
}

bool link_profile_none_imported(void)
{
    return s_imported_count == 0;
}

/*
 * Up to three decimals, trailing zeros dropped, one kept: 869.525 and
 * 869.5 are different channels, and a bare "868" reads as a count.
 *
 * Composed from integers because there is no %f on this platform
 * (CONFIG_LV_USE_CLIB_SPRINTF=n).
 */
static void format_thousandths(uint32_t value, char *buf, unsigned size)
{
    uint32_t frac = value % 1000u;
    unsigned digits = 3;
    while (digits > 1 && frac % 10u == 0) {
        frac /= 10u;
        digits--;
    }

    snprintf(buf, size, "%u.%0*u", (unsigned)(value / 1000u), (int)digits,
             (unsigned)frac);
}

void link_profile_format_bw(uint32_t bw_hz, char *buf, unsigned size)
{
    if (bw_hz % 1000u == 0) {
        snprintf(buf, size, "%u", (unsigned)(bw_hz / 1000u));
    } else {
        format_thousandths(bw_hz, buf, size);
    }
}

void link_profile_format_radio(int index, char *buf, unsigned size)
{
    link_profile_cfg_t cfg;
    link_profile_get(index, &cfg);

    char freq[12];
    char bw[12];
    format_thousandths(cfg.radio.freq_hz / 1000u, freq, sizeof(freq));
    link_profile_format_bw(cfg.radio.bw_hz, bw, sizeof(bw));

    snprintf(buf, size, "%s MHz  SF%u  %s kHz", freq, (unsigned)cfg.radio.sf,
             bw);
}

void link_profile_format_summary(int index, char *buf, unsigned size)
{
    link_profile_cfg_t cfg;
    link_profile_get(index, &cfg);

    char freq[12];
    char bw[12];
    format_thousandths(cfg.radio.freq_hz / 1000u, freq, sizeof(freq));
    link_profile_format_bw(cfg.radio.bw_hz, bw, sizeof(bw));

    snprintf(buf, size, "%s - %s MHz SF%u BW%s", link_profile_name(index),
             freq, (unsigned)cfg.radio.sf, bw);
}
