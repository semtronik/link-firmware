/**
 * @file link_battery.c
 * @brief MAX17048 fuel gauge and battery presence detection.
 */

#include "link_battery.h"

#include "board_core_bus.h"
#include "esp_log.h"

static const char *TAG = "link_battery";

/* MAX17048 registers, 16-bit big-endian. */
#define REG_VCELL   0x02 /* 78.125 uV per LSB */
#define REG_SOC     0x04 /* 1/256 % per LSB   */
#define REG_VERSION 0x08
#define REG_CRATE   0x16 /* signed, 0.208 %/h per LSB */

/*
 * A plausible cell. Outside this band nothing is fitted, whatever the
 * gauge says: below MIN a LiPo is past deep discharge, above MAX no
 * cell has terminated charge.
 */
#define CELL_MIN_MV 2500U
#define CELL_MAX_MV 4300U

/*
 * How many consecutive good samples before declaring a cell present.
 *
 * The charger's battery-detect cycle is ~500 ms. At the ~1 Hz this is
 * meant to be polled at, samples land on unrelated phases of that
 * cycle, so an open output cannot produce a long run of good readings:
 * it would have to be up at every single sample. Eight is far past
 * coincidence and costs a few seconds, which nothing here is racing.
 */
#define PRESENT_STREAK 8

/*
 * How many consecutive failures before declaring the cell gone.
 *
 * More than one, so a lone brown-out or a bus glitch does not flip a
 * steady reading - an indicator that flickers is worse than one that
 * takes a moment to settle.
 */
#define ABSENT_STREAK 3

static link_battery_t s_state;
static unsigned s_good_streak;
static unsigned s_bad_streak;

static esp_err_t read_u16(uint8_t reg, uint16_t *out)
{
    uint8_t rx[2] = {0};

    const esp_err_t err = board_core_bus_read(LINK_BATTERY_ADDR, reg, rx, sizeof(rx));
    if (err == ESP_OK) {
        *out = (uint16_t)((rx[0] << 8) | rx[1]);
    }
    return err;
}

const char *link_battery_presence_str(link_battery_presence_t presence)
{
    switch (presence) {
    case LINK_BATTERY_PRESENT:
        return "present";
    case LINK_BATTERY_ABSENT:
        return "absent";
    default:
        return "detecting";
    }
}

esp_err_t link_battery_init(void)
{
    s_state = (link_battery_t){.presence = LINK_BATTERY_UNKNOWN};
    s_good_streak = 0;
    s_bad_streak = 0;

    uint16_t version = 0;
    if (read_u16(REG_VERSION, &version) == ESP_OK) {
        ESP_LOGI(TAG, "MAX17048 version 0x%04X", version);
    } else {
        /* Not an error: with no cell fitted the gauge is unpowered for
         * part of every detect cycle, and may simply be in that half. */
        ESP_LOGI(TAG, "gauge silent at init, detection will settle it");
    }

    return ESP_OK;
}

/*
 * Commit a sample to the published state, once presence is known.
 *
 * With no cell there is no cell voltage to show, however well the gauge
 * answered: what it measured is the charger's open output. Reporting it
 * would contradict the verdict sitting next to it.
 */
static void publish(bool ok, uint32_t mv, uint8_t pct, int32_t rate)
{
    if (s_state.presence == LINK_BATTERY_ABSENT) {
        s_state.gauge_ok = false;
        s_state.vcell_mv = 0;
        s_state.soc_pct = 0;
        s_state.crate_milli_pct_h = 0;
        return;
    }

    s_state.gauge_ok = ok;
    if (ok) {
        s_state.vcell_mv = mv;
        s_state.soc_pct = pct;
        s_state.crate_milli_pct_h = rate;
    }
}

void link_battery_poll(bool usb_present)
{
    uint16_t vcell = 0, soc = 0, crate = 0;

    const bool ok = read_u16(REG_VCELL, &vcell) == ESP_OK &&
                    read_u16(REG_SOC, &soc) == ESP_OK;

    /* Held locally until presence is decided below: with no cell fitted
     * the gauge still answers during the up half of the detect cycle,
     * and what it reports then is the charger's open output, not a
     * battery. Publishing it would put a plausible voltage next to
     * "absent". */
    uint32_t mv = 0;
    uint8_t pct = 0;
    int32_t rate = 0;

    if (ok) {
        mv = (uint32_t)vcell * 78125U / 1000000U;
        pct = (uint8_t)(soc >> 8);
        if (read_u16(REG_CRATE, &crate) == ESP_OK) {
            rate = (int32_t)(int16_t)crate * 208;
        }
    }

    /*
     * Running off the cell is proof the cell is there - the board has no
     * other source. This is not a heuristic like the streaks below, so
     * it settles the question outright and does not wait for them.
     */
    if (!usb_present) {
        s_good_streak = 0;
        s_bad_streak = 0;
        if (s_state.presence != LINK_BATTERY_PRESENT) {
            ESP_LOGI(TAG, "battery present (running on it)");
            s_state.presence = LINK_BATTERY_PRESENT;
        }
        publish(ok, mv, pct, rate);
        return;
    }

    /* On USB the rail is held up either way, so presence has to be
     * inferred from how the gauge behaves over several samples. */
    const bool plausible = ok && mv >= CELL_MIN_MV && mv <= CELL_MAX_MV;

    if (plausible) {
        s_bad_streak = 0;
        if (s_good_streak < PRESENT_STREAK) {
            s_good_streak++;
        }
    } else {
        s_good_streak = 0;
        if (s_bad_streak < ABSENT_STREAK) {
            s_bad_streak++;
        }
    }

    const link_battery_presence_t was = s_state.presence;

    if (s_good_streak >= PRESENT_STREAK) {
        s_state.presence = LINK_BATTERY_PRESENT;
    } else if (s_bad_streak >= ABSENT_STREAK) {
        s_state.presence = LINK_BATTERY_ABSENT;
    }

    if (s_state.presence != was) {
        ESP_LOGI(TAG, "battery %s",
                 link_battery_presence_str(s_state.presence));
    }

    publish(ok, mv, pct, rate);
}

void link_battery_get(link_battery_t *out)
{
    if (out != NULL) {
        *out = s_state;
    }
}
