/**
 * @file sx1262_radio.h
 * @brief LoRa P2P operation on the Wio-SX1262 module.
 *
 * Sits on the primitives in sx1262.h. Frequencies are in Hz, power in
 * dBm; the module's TCXO and RF switch are handled by sx1262_radio_init().
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Largest payload this driver will send or receive. */
#define SX1262_MAX_PAYLOAD 255

/** Band the Wio-SX1262 is built for (Seeed datasheet V1.1), Hz. */
#define SX1262_FREQ_MIN_HZ 862000000U
#define SX1262_FREQ_MAX_HZ 930000000U

/** LoRa modulation and RF settings. */
typedef struct {
    uint32_t freq_hz;      /**< carrier, Hz */
    uint8_t  sf;           /**< spreading factor, 5..12 */
    uint32_t bw_hz;        /**< bandwidth, Hz */
    uint8_t  cr;           /**< coding rate denominator, 5..8 (4/5..4/8) */
    int8_t   power_dbm;    /**< -9..22 */
    uint16_t preamble_len; /**< symbols */
    bool     crc_on;
    uint8_t  sync_word;    /**< 0x12 private, 0x34 public */
    bool     iq_inverted;  /**< as LoRaWAN downlinks */
} sx1262_radio_cfg_t;

/** A received frame. */
typedef struct {
    uint8_t payload[SX1262_MAX_PAYLOAD];
    uint8_t len;
    /** Signal and noise together, averaged over the frame. */
    int16_t rssi_dbm;
    /** Quarter dB: -31 is -7.75 dB. */
    int16_t snr_qdb;
    /** SignalRssiPkt: the signal alone, after despreading. */
    int16_t signal_dbm;
    /**
     * True when the CRC failed: the payload is corrupt, everything
     * beside it is not.
     *
     * Delivered rather than dropped. A frame that arrives and fails its
     * check still carries what says why - the level it came in at, how
     * far it sat above the noise, how far off frequency the transmitter
     * was - and on an unknown link those are the measurements being
     * looked for. The payload of an encrypted protocol was never going
     * to be readable anyway.
     */
    bool    crc_error;
    /** Transmitter's offset from the tuned frequency, Hz, signed. */
    int32_t freq_error_hz;
    /** From the frame's header, as sent: 5..8 for 4/5..4/8. */
    uint8_t cr;
    bool    crc_on;
} sx1262_rx_t;

/**
 * @brief Defaults: 868.0 MHz, SF9, BW125, CR 4/5, 0 dBm, CRC on,
 *        sync 0x12, standard IQ.
 */
void sx1262_radio_default_cfg(sx1262_radio_cfg_t *cfg);

/** @brief Whether the chip and the module accept cfg. Touches nothing. */
esp_err_t sx1262_radio_check_cfg(const sx1262_radio_cfg_t *cfg);

/**
 * @brief Reset the chip and apply cfg: TCXO, calibration, LoRa mode,
 *        modulation and PA settings. Leaves the chip in standby.
 */
esp_err_t sx1262_radio_init(const sx1262_radio_cfg_t *cfg);

/** @brief Re-apply cfg without a reset. */
esp_err_t sx1262_radio_configure(const sx1262_radio_cfg_t *cfg);

/**
 * @brief Receiver gain for every later configure(): boosted, or power
 *        saving (the chip's default).
 */
void sx1262_radio_set_rx_boosted(bool boosted);

/** @brief The receiver gain configure() applies. */
bool sx1262_radio_rx_boosted(void);

/**
 * @brief Move to another frequency, keeping everything else.
 *
 * For sweeping, where the cost per channel is what decides how fast the
 * band can be covered. sx1262_radio_configure() recalibrates the image
 * on every call - several milliseconds, which over a band of channels
 * outweighs everything else put together - and image calibration is per
 * band, not per channel. This writes the PLL word and nothing else.
 *
 * The caller is responsible for having calibrated the band once, which
 * sx1262_radio_init() or a first configure() does. Staying inside the
 * calibrated band is also on the caller: 863-870 and 902-928 are
 * calibrated separately, and crossing between them needs a configure().
 *
 * Leaves the chip where it was. Called while the receiver is armed, it
 * retunes without dropping to standby.
 */
esp_err_t sx1262_radio_retune(uint32_t freq_hz);

/** Reception stages seen since the last call, as flags. */
#define SX1262_STAGE_PREAMBLE (1u << 0)
#define SX1262_STAGE_HEADER   (1u << 1)
#define SX1262_STAGE_HEADER_ERR (1u << 2)

/**
 * @brief Read and consume the stages a reception has reached.
 *
 * What separates one kind of failure from another. A preamble seen
 * means the frequency, spreading factor and bandwidth are right - the
 * correlator will not lock otherwise - whether or not a frame follows.
 * A valid header means the sync word matches too. Neither says anything
 * arrived intact; that is what rx_collect() is for.
 *
 * Cleared as they are read, so each call reports what happened since
 * the last one.
 */
esp_err_t sx1262_radio_rx_stages(uint32_t *out_flags);

/** IRQ status bits, as sx1262_radio_irq_take() reports them. */
#define SX1262_IRQ_TX_DONE      (1u << 0)
#define SX1262_IRQ_RX_DONE      (1u << 1)
#define SX1262_IRQ_PREAMBLE_DET (1u << 2)
#define SX1262_IRQ_HEADER_VALID (1u << 4)
#define SX1262_IRQ_HEADER_ERR   (1u << 5)
#define SX1262_IRQ_CRC_ERR      (1u << 6)
#define SX1262_IRQ_TIMEOUT      (1u << 9)

/**
 * @brief Read the IRQ status and clear every bit found set.
 *
 * Exclusive with rx_stages(), rx_collect() and tx_done(), which rely on
 * the flags.
 */
esp_err_t sx1262_radio_irq_take(uint16_t *out_irq);

/**
 * @brief Read out the frame in the receive buffer, no flag checked.
 *
 * crc_error is left false: the caller has it from irq_take().
 */
esp_err_t sx1262_radio_rx_read(sx1262_rx_t *out);

/** @brief Coding rate and CRC presence of the last header received. */
esp_err_t sx1262_radio_rx_header(uint8_t *out_cr, bool *out_crc_on);

/** @brief Settings currently applied. */
const sx1262_radio_cfg_t *sx1262_radio_get_cfg(void);

/**
 * @brief Put one frame on air and return at once.
 *
 * TX done and timeout are routed to DIO1; poll sx1262_radio_tx_done()
 * when it rises. The caller times the transmission: a chip that never
 * raises TX done points at the PLL or the TCXO rather than the payload.
 */
esp_err_t sx1262_radio_tx_start(const uint8_t *payload, size_t len);

/**
 * @brief Where the transmission started by tx_start() stands.
 *
 * @return ESP_OK once sent, ESP_ERR_NOT_FINISHED while on air,
 *         ESP_ERR_INVALID_RESPONSE if the chip gave up - told apart from
 *         ESP_ERR_TIMEOUT, which is the bus not answering. The chip is
 *         back in standby after either end.
 */
esp_err_t sx1262_radio_tx_done(void);

/**
 * @brief Put the receiver in continuous mode and leave it there.
 *
 * Pair with sx1262_radio_rx_collect(); the receiver stays armed across
 * frames, so nothing is missed between them.
 */
esp_err_t sx1262_radio_rx_start(void);

/**
 * @brief Read a frame the armed receiver has captured.
 *
 * @return ESP_ERR_NOT_FOUND if no frame is pending, ESP_ERR_INVALID_CRC
 *         if one arrived with a bad CRC. The receiver stays armed in
 *         both cases.
 */
esp_err_t sx1262_radio_rx_collect(sx1262_rx_t *out);

/** @brief Back to standby, receiver off. */
esp_err_t sx1262_radio_standby(void);

/**
 * @brief One instantaneous RSSI reading, dBm.
 *
 * Reads while the receiver runs and leaves the mode alone, so a caller
 * watching continuously keeps the receiver armed between readings
 * rather than paying an AGC settle each time. The receiver must already
 * be on - arm it with sx1262_radio_rx_start().
 *
 * @return ESP_ERR_INVALID_RESPONSE at the bottom of the scale, which is
 *         what the chip gives when it has no value rather than a
 *         reading of an exceptionally quiet channel.
 */
esp_err_t sx1262_radio_rssi_inst(int16_t *out_dbm);

/** @brief Time on air for a payload of len bytes, microseconds. */
uint32_t sx1262_radio_time_on_air_us(const sx1262_radio_cfg_t *cfg,
                                     size_t len);

#ifdef __cplusplus
}
#endif
