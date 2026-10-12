#include "link_regul.h"

#include <math.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "link_regul";

/* ERC 70-03 Annex 1. 25 mW is 14 dBm, 500 mW 27 dBm, both e.r.p.; duty
 * cycles in per mille. h1.8 (5 mW, no duty cycle) shares 869.7-870 with
 * h1.9 and is left out: a transmitter at 25 mW is under h1.9. */
typedef struct {
    const char *name;
    uint32_t lo_hz;
    uint32_t hi_hz;
    int8_t power_dbm;
    uint16_t duty_pm;
} eu_band_t;

static const eu_band_t EU_BANDS[] = {
    {"h0", 862000000U, 863000000U, 14, 1},
    {"h1.3", 863000000U, 865000000U, 14, 1},
    {"h1.4", 865000000U, 868000000U, 14, 10},
    {"h1.5", 868000000U, 868600000U, 14, 10},
    {"h1.6", 868700000U, 869200000U, 14, 1},
    {"h1.7", 869400000U, 869650000U, 27, 100},
    {"h1.9", 869700000U, 870000000U, 14, 10},
    /* Whatever else fits in 863-870: 0.1%, and a density limit below. */
    {"h1.2", 863000000U, 870000000U, 14, 1},
};

#define EU_BAND_COUNT (sizeof(EU_BANDS) / sizeof(EU_BANDS[0]))
#define EU_H12        (EU_BAND_COUNT - 1)

/* h1.2 also caps density at -4.5 dBm per 100 kHz e.r.p. */
#define EU_H12_PSD_DBM_100K (-4.5f)

/* Annex 7, excluded from Annex 1 (its note 2). */
static const uint32_t EU_ALARMS[][2] = {
    {868600000U, 868700000U},
    {869200000U, 869400000U},
    {869650000U, 869700000U},
};

#define US_LO_HZ 902000000U
#define US_HI_HZ 928000000U

/* 15.247(a)(1)(i): 0.4 s on a channel. (b)(1)-(3): 1 W conducted, or
 * 0.25 W for a hopping system of 25-49 channels, which is what a
 * 250 kHz channel allows. */
#define US_DWELL_MS      400U
#define US_POWER_DBM     30
#define US_POWER_WIDE_DBM 24

/* Transmit time per band, in one-minute slots over the last hour. */
#define SLOTS 60

static SemaphoreHandle_t s_lock;
static link_region_t s_region = LINK_REGION_EU868;
static int32_t s_slot_min[SLOTS];
static uint32_t s_slot_ms[EU_BAND_COUNT][SLOTS];

static int32_t now_min(void)
{
    return (int32_t)(esp_timer_get_time() / 60000000LL);
}

static void forget_time(void)
{
    for (int i = 0; i < SLOTS; i++) {
        s_slot_min[i] = INT32_MIN / 2;
    }
    memset(s_slot_ms, 0, sizeof(s_slot_ms));
}

esp_err_t link_regul_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    forget_time();
    return ESP_OK;
}

link_region_t link_regul_get_region(void)
{
    return s_region;
}

esp_err_t link_regul_set_region(link_region_t region)
{
    if (region >= LINK_REGION_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Every profile applied names a region: the hour's count only goes
     * when it actually changes, or a profile switch would hide an
     * exceeded budget. */
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (region != s_region) {
        s_region = region;
        forget_time();
        ESP_LOGI(TAG, "region %s", link_regul_region_name(region));
    }
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

static const char *const REGION_NAMES[LINK_REGION_COUNT] = {
    [LINK_REGION_EU868] = "EU868",
    [LINK_REGION_US915] = "US915",
};

const char *link_regul_region_name(link_region_t region)
{
    return region < LINK_REGION_COUNT ? REGION_NAMES[region] : "?";
}

link_region_t link_regul_region_from_name(const char *name)
{
    for (int i = 0; i < LINK_REGION_COUNT; i++) {
        if (name != NULL && strcmp(name, REGION_NAMES[i]) == 0) {
            return (link_region_t)i;
        }
    }
    return LINK_REGION_COUNT;
}

static bool overlaps(uint32_t lo, uint32_t hi, uint32_t a, uint32_t b)
{
    return lo < b && hi > a;
}

/*
 * The EU band a frame falls under, from the edges of what it occupies:
 * the whole signal has to fit, not just its centre. -1 when none does.
 */
static int eu_band_of(uint32_t freq_hz, uint32_t bw_hz, bool *alarm)
{
    const uint32_t lo = freq_hz - bw_hz / 2;
    const uint32_t hi = freq_hz + bw_hz / 2;

    *alarm = false;
    for (size_t i = 0; i < sizeof(EU_ALARMS) / sizeof(EU_ALARMS[0]); i++) {
        if (overlaps(lo, hi, EU_ALARMS[i][0], EU_ALARMS[i][1])) {
            *alarm = true;
            return -1;
        }
    }

    /* The specific bands first; h1.2 catches what straddles them. */
    for (size_t i = 0; i < EU_BAND_COUNT; i++) {
        if (lo >= EU_BANDS[i].lo_hz && hi <= EU_BANDS[i].hi_hz) {
            return (int)i;
        }
    }
    return -1;
}

void link_regul_record(uint32_t freq_hz, uint32_t bw_hz, uint32_t on_air_us)
{
    if (s_lock == NULL || s_region != LINK_REGION_EU868) {
        return;
    }

    bool alarm;
    const int band = eu_band_of(freq_hz, bw_hz, &alarm);
    if (band < 0) {
        return;
    }

    const int32_t minute = now_min();
    const int slot = (int)(minute % SLOTS);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_slot_min[slot] != minute) {
        s_slot_min[slot] = minute;
        for (size_t b = 0; b < EU_BAND_COUNT; b++) {
            s_slot_ms[b][slot] = 0;
        }
    }
    s_slot_ms[band][slot] += (on_air_us + 999) / 1000;
    xSemaphoreGive(s_lock);
}

static uint32_t used_last_hour_ms(int band)
{
    const int32_t minute = now_min();
    uint32_t sum = 0;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int s = 0; s < SLOTS; s++) {
        if (minute - s_slot_min[s] < SLOTS) {
            sum += s_slot_ms[band][s];
        }
    }
    xSemaphoreGive(s_lock);

    return sum;
}

static void assess_eu(uint32_t freq_hz, uint32_t bw_hz, int8_t power_dbm,
                      uint32_t pending_us, link_regul_t *out)
{
    bool alarm;
    const int band = eu_band_of(freq_hz, bw_hz, &alarm);

    if (band < 0) {
        out->warn |= alarm ? LINK_REGUL_W_ALARM_BAND
                           : LINK_REGUL_W_OUT_OF_BAND;
        return;
    }

    const eu_band_t *b = &EU_BANDS[band];
    out->band = b->name;
    out->band_lo_hz = b->lo_hz;
    out->band_hi_hz = b->hi_hz;
    out->power_ref = LINK_REGUL_REF_ERP;
    out->power_limit_dbm = b->power_dbm;

    if (band == (int)EU_H12) {
        const float psd_cap =
            EU_H12_PSD_DBM_100K + 10.0f * log10f((float)bw_hz / 100000.0f);
        const int8_t cap = (int8_t)floorf(psd_cap);
        if (cap < out->power_limit_dbm) {
            out->power_limit_dbm = cap;
        }
    }

    out->duty_limit_pm = b->duty_pm;
    out->duty_budget_ms = (uint32_t)b->duty_pm * 3600U;
    out->duty_used_ms = used_last_hour_ms(band) + (pending_us + 999) / 1000;

    if (power_dbm > out->power_limit_dbm) {
        out->warn |= LINK_REGUL_W_POWER;
    }
    if (out->duty_used_ms > out->duty_budget_ms) {
        out->warn |= LINK_REGUL_W_DUTY;
    }
}

static void assess_us(uint32_t freq_hz, uint32_t bw_hz, int8_t power_dbm,
                      uint32_t toa_us, link_regul_t *out)
{
    const uint32_t lo = freq_hz - bw_hz / 2;
    const uint32_t hi = freq_hz + bw_hz / 2;

    if (lo < US_LO_HZ || hi > US_HI_HZ) {
        out->warn |= LINK_REGUL_W_OUT_OF_BAND;
        return;
    }

    out->band_lo_hz = US_LO_HZ;
    out->band_hi_hz = US_HI_HZ;
    out->power_ref = LINK_REGUL_REF_CONDUCTED;

    /* (a)(2): digital modulation needs 500 kHz. Anything narrower on one
     * channel is not a hopping system either, which is the rule it would
     * otherwise fall under. */
    if (bw_hz >= 500000U) {
        out->band = "DTS";
        out->power_limit_dbm = US_POWER_DBM;
    } else {
        out->band = "fixed";
        out->power_limit_dbm =
            bw_hz >= 250000U ? US_POWER_WIDE_DBM : US_POWER_DBM;
        out->dwell_limit_ms = US_DWELL_MS;
        out->warn |= LINK_REGUL_W_NOT_HOPPING;
        if (toa_us > US_DWELL_MS * 1000U) {
            out->warn |= LINK_REGUL_W_DWELL;
        }
    }

    if (power_dbm > out->power_limit_dbm) {
        out->warn |= LINK_REGUL_W_POWER;
    }
}

void link_regul_assess(uint32_t freq_hz, uint32_t bw_hz, int8_t power_dbm,
                       uint32_t toa_us, uint32_t pending_us,
                       link_regul_t *out)
{
    memset(out, 0, sizeof(*out));
    out->region = s_region;

    if (s_lock == NULL) {
        return;
    }

    if (s_region == LINK_REGION_US915) {
        assess_us(freq_hz, bw_hz, power_dbm, toa_us, out);
    } else {
        assess_eu(freq_hz, bw_hz, power_dbm, pending_us, out);
    }
}
