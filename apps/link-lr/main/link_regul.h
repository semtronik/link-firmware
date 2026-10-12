/**
 * @file link_regul.h
 * @brief What the regulations say about a transmission. Informs, never
 *        restricts.
 *
 * This is a diagnostic tool: it must be able to send what the device
 * under test sends, compliant or not. So nothing here stops a frame - it
 * describes where the frame sits in the region's band plan, what the
 * limits there are, and how much of the hour's transmit budget has gone.
 *
 * The region comes with the profile applied, EU868 until one is. A
 * frequency does not name one: 863-870 is Europe but also Russia and India, 915-928 the US
 * but also Australia, each under different rules.
 *
 * Sources, checked against the texts rather than recalled:
 *   EU868  ERC Recommendation 70-03, Annex 1 and Appendix 5, Feb. 2025
 *   US915  47 CFR 15.247 (a), (b), (e)
 *
 * Three limits, to be shown as such:
 *   - power is the conducted power set on the chip; the EU limits are
 *     e.r.p., and the antenna in between is unknown here
 *   - the transmit time counted restarts at every boot
 *   - LBT+AFA is not implemented, so the duty cycle rule is the one
 *     that applies
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LINK_REGION_EU868,
    LINK_REGION_US915,
    LINK_REGION_COUNT,
} link_region_t;

/** Warnings, as flags. None of them stops anything. */
#define LINK_REGUL_W_OUT_OF_BAND  (1u << 0) /**< outside the region's plan */
#define LINK_REGUL_W_ALARM_BAND   (1u << 1) /**< EU: reserved for alarms */
#define LINK_REGUL_W_POWER        (1u << 2) /**< set power above the limit */
#define LINK_REGUL_W_DUTY         (1u << 3) /**< hour's budget exceeded */
#define LINK_REGUL_W_DWELL        (1u << 4) /**< US: frame over 400 ms */
#define LINK_REGUL_W_NOT_HOPPING  (1u << 5) /**< US: fixed channel < 500 kHz */

/** How a limit is expressed in the texts. */
typedef enum {
    LINK_REGUL_REF_CONDUCTED,
    LINK_REGUL_REF_ERP,
} link_regul_ref_t;

typedef struct {
    link_region_t region;
    /** Sub-band or mode the frame falls under ("h1.5", "DTS"...), or NULL. */
    const char *band;
    /** Its edges, Hz; 0 when there is no band. */
    uint32_t band_lo_hz;
    uint32_t band_hi_hz;
    /** Power limit there, dBm, and what it is measured as. */
    int8_t power_limit_dbm;
    link_regul_ref_t power_ref;
    /** Duty cycle limit, per mille; 0 when the band has none. */
    uint16_t duty_limit_pm;
    /** Time on air in this band over the last hour, and the allowance. */
    uint32_t duty_used_ms;
    uint32_t duty_budget_ms;
    /** Longest a frame may last there, ms; 0 when there is no such rule. */
    uint32_t dwell_limit_ms;
    uint32_t warn;
} link_regul_t;

/** @brief Call before the first profile is applied. */
esp_err_t link_regul_init(void);

link_region_t link_regul_get_region(void);

/**
 * @brief Set the region. The transmit time counted so far is forgotten
 *        only if it changes.
 */
esp_err_t link_regul_set_region(link_region_t region);

/** @brief "EU868", "US915". */
const char *link_regul_region_name(link_region_t region);

/** @brief The region named so, or LINK_REGION_COUNT if unknown. */
link_region_t link_regul_region_from_name(const char *name);

/**
 * @brief Count a transmission against its band.
 *
 * @param on_air_us  How long it was actually on air.
 */
void link_regul_record(uint32_t freq_hz, uint32_t bw_hz, uint32_t on_air_us);

/**
 * @brief Where a frame sits and what applies to it.
 *
 * @param toa_us      the frame's time on air, for the per-frame rules
 * @param pending_us  time not recorded yet to count in the hour: the
 *                    frame's own before it is sent, 0 once recorded
 */
void link_regul_assess(uint32_t freq_hz, uint32_t bw_hz, int8_t power_dbm,
                       uint32_t toa_us, uint32_t pending_us,
                       link_regul_t *out);

#ifdef __cplusplus
}
#endif
