/**
 * @file link_radio.h
 * @brief The radio service, and the snapshot the screens read.
 *
 * One task owns the chip. Everything else asks it - configure, lend the
 * chip to the band sweep and take it back, send a frame - and it acts
 * between two reads. The signal sampler alone reads RSSI directly, a
 * command with no effect on the chip's state.
 *
 * The receiver stays armed, and every reception stage raises DIO1. The
 * radio's buffer holds a single frame and is overwritten by the next, so
 * the gap between the interrupt and the read decides how many of a
 * burst survive.
 *
 * A chip that stops answering is reset and brought back by the task.
 *
 * It keeps more than the link needs, because the same path serves the
 * diagnostic screen: the stages a transmission reached before it was
 * complete, frames whose CRC failed, and how far off frequency the
 * transmitter sat. None of that costs anything to collect - it is
 * already in the chip when a frame is read out - and what it buys is
 * the ability to say why nothing is arriving, which counters of
 * successes never can.
 *
 * Screens never call the driver. They take a snapshot, which is
 * consistent by construction: one lock, one struct copy.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "sx1262_radio.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Bytes of payload kept.
 *
 * The whole of it: 255 is what a LoRa length field can express, so
 * nothing is ever truncated. This is an analysis tool, and a frame cut
 * short at the last moment is the kind of silent loss that costs an
 * afternoon - the bytes have already been paid for by the time they
 * reach here, and dropping them buys nothing but the ~1 kB the state
 * grows by.
 */
#define LINK_RADIO_PAYLOAD_MAX SX1262_MAX_PAYLOAD

typedef struct {
    /**
     * Wall-clock stamp of the frame's first symbol, epoch milliseconds:
     * its end less its time on air. Never revised - a later clock re-sync
     * does not rewrite it. 0 when the clock was not set.
     */
    int64_t epoch_ms;
    /**
     * Milliseconds since boot. What ages are measured against: it is
     * immune to the clock moving, which the epoch stamp is not. 0 if
     * nothing has been received.
     */
    int64_t ts_ms;
    /** Since the frame before it, or -1 for the first one. */
    int64_t delta_ms;
    int16_t rssi_dbm;
    /** Quarter dB. */
    int16_t snr_qdb;
    /** The signal alone (SignalRssiPkt); not shown until characterised. */
    int16_t signal_dbm;
    /**
     * Transmitter's offset from the tuned frequency, Hz, signed.
     *
     * Read from the chip's own estimate, which is valid on a frame that
     * failed its CRC as much as on one that passed. What it is for: a
     * frequency found to within a channel width can be refined to
     * within a few hundred hertz by listening once and reading this.
     */
    int32_t freq_error_hz;
    /**
     * True when the CRC failed: the payload is corrupt, the
     * measurements beside it are not.
     *
     * Kept rather than dropped. A frame that arrives and does not check
     * out still says how strong it was, how far above the noise, and
     * how far off frequency - which is exactly what is wanted when the
     * settings are still being worked out, and what a bare drop count
     * cannot give.
     */
    bool crc_error;
    /** Stage instants, us since boot; 0 if the stage did not happen. */
    int64_t preamble_us;
    int64_t header_us;
    int64_t end_us;
    /** Computed from the frame's own header; 0 if nothing was received. */
    uint32_t toa_us;
    /** Instantaneous RSSI sampled during the frame; inst_n 0 if none. */
    uint16_t inst_n;
    int16_t inst_min_dbm;
    int16_t inst_max_dbm;
    /** Median instantaneous RSSI over the last moments before the frame;
     *  noise_n 0 if none. */
    uint8_t noise_n;
    int16_t noise_dbm;
    /**
     * The settings it was received or sent on. For a reception, cr and
     * crc_on come from the frame's own header; cr is 0 when none was read.
     */
    sx1262_radio_cfg_t radio;
    uint8_t len;                              /**< as received, before truncation */
    uint8_t payload[LINK_RADIO_PAYLOAD_MAX];  /**< first bytes of it */
    uint8_t kept;                             /**< how many of them are here */
} link_radio_frame_t;

/** Entries kept in the reception log, newest first. */
#define LINK_RADIO_LOG 4

/** How a logged reception ended. */
typedef enum {
    /** A header passed but nothing complete followed it. */
    LINK_RADIO_ENTRY_LOST,
    /** A frame arrived and checked out. */
    LINK_RADIO_ENTRY_FRAME,
    /** A frame arrived and its CRC failed. */
    LINK_RADIO_ENTRY_CRC,
    /** A header arrived and failed its own check. */
    LINK_RADIO_ENTRY_HEADER_ERR,
} link_radio_entry_kind_t;

typedef struct {
    link_radio_entry_kind_t kind;
    /** Boot milliseconds when the reception started. */
    int64_t at_ms;
    /** A LOST entry carries only its header's stamps. */
    link_radio_frame_t frame;
} link_radio_entry_t;

/** How a transmission ended. */
typedef enum {
    LINK_RADIO_TX_OK,
    /** TX done never came: PLL or TCXO rather than the payload. */
    LINK_RADIO_TX_TIMEOUT,
    /** Cut short by a reconfiguration or by the chip being lent out. */
    LINK_RADIO_TX_ABORTED,
} link_radio_tx_result_t;

/** The last transmission that ended, and the settings it went out on. */
typedef struct {
    /** Bumped each time one ends; 0 until the first. */
    uint32_t seq;
    link_radio_tx_result_t result;
    /** Wall-clock stamp of the start, epoch ms; 0 if the clock was unset. */
    int64_t epoch_ms;
    /** The same instant, ms since boot. */
    int64_t ts_ms;
    uint8_t len;
    uint8_t payload[SX1262_MAX_PAYLOAD];
    uint32_t toa_us;
    sx1262_radio_cfg_t radio;
} link_radio_tx_t;

typedef struct {
    /** False when the receiver could not be armed. */
    bool listening;
    /** A frame of ours is on air; nothing is received meanwhile. */
    bool transmitting;
    /** Times the chip stopped answering and was reset back to work. */
    uint32_t recoveries;
    /** Worst edge-to-clear delay; must stay well under ~300 us. */
    uint32_t irq_latency_max_us;
    /** Stamps that are not their event's own; must stay at 0. */
    uint32_t irq_anomalies;
    link_radio_tx_t tx;
    /** Frames received since boot. Never wraps in any realistic run. */
    uint32_t rx_count;
    /**
     * rx_count and crc_errors again, never reset: what a reader compares
     * to find what is new since it last looked. The two above go back to
     * zero on link_radio_reset_stats(), which would read as a frame.
     */
    uint32_t rx_seq;
    uint32_t crc_seq;
    /** Frames whose CRC failed. */
    uint32_t crc_errors;
    /** The most recent frame that checked out. ts_ms is 0 until one arrives. */
    link_radio_frame_t last;

    /** Recent receptions, newest first, failures included. */
    link_radio_entry_t entry[LINK_RADIO_LOG];
    uint8_t entries;
    /** Bumped with each entry, never reset. */
    uint32_t entry_seq;
} link_radio_state_t;

/** What the radio reports to its listener, one event at a time. */
typedef enum {
    LINK_RADIO_EVENT_RX,
    LINK_RADIO_EVENT_TX,
    /** Reception stopped while the chip was being reset. */
    LINK_RADIO_EVENT_GAP,
} link_radio_event_kind_t;

typedef struct {
    link_radio_event_kind_t kind;
    union {
        link_radio_entry_t rx;
        link_radio_tx_t tx;
        struct {
            /** Wall clock, epoch ms; 0 when the clock was unset. */
            int64_t from_ms;
            int64_t to_ms;
        } gap;
    };
} link_radio_event_t;

/**
 * @brief Called on the radio task for every event, so it must not block:
 *        copy and return.
 */
typedef void (*link_radio_listener_t)(const link_radio_event_t *ev);

/** @brief One listener; NULL to remove. Sees only what follows the call. */
void link_radio_set_listener(link_radio_listener_t listener);

/**
 * @brief Bring up the radio and start listening.
 *
 * Applies @p cfg and the receiver gain, arms the receiver and starts the
 * task. Pass NULL for the driver defaults.
 */
esp_err_t link_radio_start(const sx1262_radio_cfg_t *cfg, bool rx_boosted);

/**
 * @brief Apply new settings and receiver gain without losing the receiver.
 *
 * Done by the receive task between two reads, which then re-arms the
 * receiver. Both are taken or neither is. While the chip is lent out
 * (link_radio_suspend), they are only checked and kept, and applied on
 * resume. Counters and the last frame are left alone - they describe
 * what was received, not how the radio is set.
 */
esp_err_t link_radio_configure(const sx1262_radio_cfg_t *cfg, bool rx_boosted);

/**
 * @brief Stop touching the radio and return once the task is out of it.
 *
 * For whoever needs the chip to itself - the band sweep retunes
 * continuously, which cannot be done under a receiver armed on a fixed
 * frequency. Counters and the last frame are left alone.
 *
 * Nothing is received until link_radio_resume(), by design: the sweep
 * runs precisely when it is not yet known what to listen for.
 */
void link_radio_suspend(void);

/**
 * @brief Re-apply the settings and re-arm the receiver.
 *
 * Undoes link_radio_suspend(), including whatever the borrower left on
 * the chip.
 */
esp_err_t link_radio_resume(void);

/**
 * @brief Send one frame on the current settings, without waiting for it.
 *
 * Returns once the frame is on air, or refused. How it ended shows up
 * later in the state's tx, whose seq moves; the receiver is re-armed as
 * soon as it is out. A reconfiguration or link_radio_suspend() meanwhile
 * cuts it short.
 *
 * @return ESP_ERR_INVALID_STATE with no module, ESP_ERR_NOT_ALLOWED while
 *         the chip is lent out, ESP_ERR_NOT_FINISHED while the previous
 *         frame is still on air, ESP_ERR_INVALID_ARG for a bad length.
 */
esp_err_t link_radio_tx_submit(const uint8_t *payload, size_t len);

/**
 * @brief Forget the counters and the log.
 *
 * For the diagnostic screen, where what matters is what happened since
 * the device under test was last triggered. The settings and the
 * receiver are untouched.
 */
void link_radio_reset_stats(void);

/** @brief Consistent copy of the current state. */
void link_radio_get(link_radio_state_t *out);

/** @brief Demodulation limit of a spreading factor, tenths of dB. */
int32_t link_radio_snr_limit_tenths(uint8_t sf);

/** @brief SNR over the demodulation limit of the frame's SF, tenths of dB. */
int32_t link_radio_margin_tenths(const link_radio_frame_t *f);

/** @brief SNR in tenths of dB, rounded half away from zero. */
int32_t link_radio_snr_tenths(const link_radio_frame_t *f);

/* 10 dB is Semtech's install margin; under 3 dB a passer-by loses frames. */
#define LINK_RADIO_MARGIN_GOOD 100
#define LINK_RADIO_MARGIN_FAIR 30

/**
 * @brief rx_seq and crc_seq alone, consistently.
 *
 * For callers that only need to know whether something arrived since
 * they last looked, and that ask often. link_radio_get() would answer
 * the same question, but it copies the whole state - over a kilobyte,
 * most of it payload buffers - and the signal sampler asks a hundred
 * times a second. Two words under the same lock instead.
 *
 * Either pointer may be NULL.
 */
void link_radio_rx_counters(uint32_t *out_rx, uint32_t *out_crc);

#ifdef __cplusplus
}
#endif
