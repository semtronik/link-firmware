/**
 * @file sx1262_radio.c
 * @brief LoRa P2P operation on the Wio-SX1262 module.
 */

#include "sx1262_radio.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sx1262.h"

static const char *TAG = "sx1262_radio";

#define CMD_SET_STANDBY              0x80
#define CMD_SET_TX                   0x83
#define CMD_SET_RX                   0x82
#define CMD_SET_RF_FREQUENCY         0x86
#define CMD_SET_PACKET_TYPE          0x8A
#define CMD_SET_TX_PARAMS            0x8E
#define CMD_SET_PA_CONFIG            0x95
#define CMD_SET_BUFFER_BASE_ADDRESS  0x8F
#define CMD_SET_MODULATION_PARAMS    0x8B
#define CMD_SET_PACKET_PARAMS        0x8C
#define CMD_SET_DIO_IRQ_PARAMS       0x08
#define CMD_GET_IRQ_STATUS           0x12
#define CMD_CLEAR_IRQ_STATUS         0x02
#define CMD_SET_DIO2_AS_RF_SWITCH    0x9D
#define CMD_SET_DIO3_AS_TCXO_CTRL    0x97
#define CMD_CALIBRATE                0x89
#define CMD_CALIBRATE_IMAGE          0x98
#define CMD_WRITE_BUFFER             0x0E
#define CMD_GET_RX_BUFFER_STATUS     0x13
#define CMD_GET_PACKET_STATUS        0x14
#define CMD_GET_RSSI_INST            0x15

#define IRQ_TX_DONE      (1u << 0)
#define IRQ_RX_DONE      (1u << 1)
#define IRQ_PREAMBLE_DET (1u << 2)
#define IRQ_SYNC_VALID   (1u << 3)
#define IRQ_HEADER_VALID (1u << 4)
#define IRQ_HEADER_ERR   (1u << 5)
#define IRQ_CRC_ERR      (1u << 6)
#define IRQ_TIMEOUT      (1u << 9)

#define PACKET_TYPE_LORA 0x01

/* 32 MHz / 2^25, the PLL step the frequency word is expressed in. */
#define FREQ_STEP_NUM 32000000ULL
#define FREQ_STEP_DEN (1ULL << 25)

/* Register holding the LoRa sync word, and the over-current limit. */
#define REG_LORA_SYNCWORD 0x0740
#define REG_OCP           0x08E7

/* Datasheet DS.SX1261-2 Rev 1.2, chapter 15 workarounds. */
#define REG_TX_MODULATION 0x0889
#define REG_TX_CLAMP      0x08D8
#define REG_IQ_POLARITY   0x0736

/* Frequency error estimate, three bytes, twenty signed bits. Valid
 * after a LoRa reception - including one whose CRC failed, which is
 * where it is most useful: a frame that arrived but did not check out
 * still says how far off the transmitter was. */
#define REG_FREQ_ERROR 0x076B

/* Coding rate of the last header received, bits 6..4; the CRC-present bit
 * is bit 4 of REG_FREQ_ERROR's first byte. */
#define REG_LORA_RX_CR 0x0749

/* Undocumented, bit 0 set: keeps the AGC from locking at low gain after
 * strong signals (Heltec/Semtech, as Meshtastic). Cleared by calibration. */
#define REG_RX_AGC_PATCH 0x08B5

/* DS 9.6, table 9-3. */
#define REG_RX_GAIN     0x08AC
#define RX_GAIN_SAVING  0x94
#define RX_GAIN_BOOSTED 0x96

static sx1262_radio_cfg_t s_cfg;
static bool s_configured = false;
static bool s_rx_boosted = false;

void sx1262_radio_set_rx_boosted(bool boosted)
{
    s_rx_boosted = boosted;
}

bool sx1262_radio_rx_boosted(void)
{
    return s_rx_boosted;
}

void sx1262_radio_default_cfg(sx1262_radio_cfg_t *cfg)
{
    cfg->freq_hz = 868000000U;
    cfg->sf = 9;
    cfg->bw_hz = 125000U;
    cfg->cr = 5;
    cfg->power_dbm = 0;
    cfg->preamble_len = 8;
    cfg->crc_on = true;
    cfg->sync_word = 0x12;
    cfg->iq_inverted = false;
}

const sx1262_radio_cfg_t *sx1262_radio_get_cfg(void)
{
    return s_configured ? &s_cfg : NULL;
}

static esp_err_t set_standby_rc(void)
{
    const uint8_t arg = 0x00;
    return sx1262_cmd_write(CMD_SET_STANDBY, &arg, 1);
}

/* The bandwidth encoding is a lookup, not an arithmetic mapping. */
static esp_err_t encode_bw(uint32_t bw_hz, uint8_t *out)
{
    static const struct {
        uint32_t hz;
        uint8_t code;
    } k_bw[] = {
        {7800, 0x00},  {10400, 0x08}, {15600, 0x01}, {20800, 0x09},
        {31250, 0x02}, {41700, 0x0A}, {62500, 0x03}, {125000, 0x04},
        {250000, 0x05}, {500000, 0x06},
    };

    for (size_t i = 0; i < sizeof(k_bw) / sizeof(k_bw[0]); i++) {
        if (k_bw[i].hz == bw_hz) {
            *out = k_bw[i].code;
            return ESP_OK;
        }
    }
    return ESP_ERR_INVALID_ARG;
}

esp_err_t sx1262_radio_check_cfg(const sx1262_radio_cfg_t *cfg)
{
    uint8_t bw;

    if (cfg->sf < 5 || cfg->sf > 12) {
        ESP_LOGE(TAG, "sf %u out of range (5..12)", cfg->sf);
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg->cr < 5 || cfg->cr > 8) {
        ESP_LOGE(TAG, "cr 4/%u out of range (4/5..4/8)", cfg->cr);
        return ESP_ERR_INVALID_ARG;
    }
    if (encode_bw(cfg->bw_hz, &bw) != ESP_OK) {
        ESP_LOGE(TAG, "bandwidth %lu Hz is not one of the supported steps",
                 (unsigned long)cfg->bw_hz);
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg->power_dbm < -9 || cfg->power_dbm > 22) {
        ESP_LOGE(TAG, "power %d dBm out of range (-9..22)", cfg->power_dbm);
        return ESP_ERR_INVALID_ARG;
    }
    /* The chip tunes 150-960 MHz, but the module is matched for its own
     * band only. */
    if (cfg->freq_hz < SX1262_FREQ_MIN_HZ || cfg->freq_hz > SX1262_FREQ_MAX_HZ) {
        ESP_LOGE(TAG, "frequency %lu Hz out of the module band",
                 (unsigned long)cfg->freq_hz);
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static esp_err_t set_frequency(uint32_t hz)
{
    const uint32_t word =
        (uint32_t)(((uint64_t)hz * FREQ_STEP_DEN) / FREQ_STEP_NUM);
    const uint8_t args[4] = {
        (uint8_t)(word >> 24), (uint8_t)(word >> 16),
        (uint8_t)(word >> 8), (uint8_t)word,
    };
    return sx1262_cmd_write(CMD_SET_RF_FREQUENCY, args, sizeof(args));
}

/*
 * Image calibration takes a band, not a frequency: 902-928 from 900 MHz
 * up, 863-870 below - the two bands the module is built for.
 */
static esp_err_t calibrate_image(uint32_t hz)
{
    uint8_t args[2] = {0xE1, 0xE9}; /* 902-928 MHz */

    if (hz < 900000000U) {
        args[0] = 0xD7; /* 863-870 MHz */
        args[1] = 0xDB;
    }
    return sx1262_cmd_write(CMD_CALIBRATE_IMAGE, args, sizeof(args));
}

/*
 * PA settings for the SX1262 at +22 dBm, from the datasheet's table.
 * paDutyCycle and hpMax are not free parameters: the combinations are
 * fixed per target power, and exceeding them damages the PA.
 */
static esp_err_t set_pa(int8_t power_dbm)
{
    const uint8_t pa_cfg[4] = {0x04, 0x07, 0x00, 0x01};
    esp_err_t err = sx1262_cmd_write(CMD_SET_PA_CONFIG, pa_cfg, sizeof(pa_cfg));
    if (err != ESP_OK) {
        return err;
    }

    /* Datasheet asks for OCP 0x38 (140 mA) whenever PA config is set. */
    const uint8_t ocp = 0x38;
    err = sx1262_reg_write(REG_OCP, &ocp, 1);
    if (err != ESP_OK) {
        return err;
    }

    /* Ramp time 200 us (0x04). */
    const uint8_t tx_params[2] = {(uint8_t)power_dbm, 0x04};
    return sx1262_cmd_write(CMD_SET_TX_PARAMS, tx_params, sizeof(tx_params));
}

/*
 * Low data rate optimisation is mandatory when a symbol lasts longer
 * than 16.38 ms, otherwise the crystal drifts within one symbol.
 */
static bool needs_ldro(uint8_t sf, uint32_t bw_hz)
{
    const uint32_t symbol_us = ((1u << sf) * 1000000u) / bw_hz;
    return symbol_us > 16380u;
}

/* Read-modify-write of one register. */
static esp_err_t reg_update(uint16_t addr, uint8_t mask, uint8_t value)
{
    uint8_t reg = 0;
    esp_err_t err = sx1262_reg_read(addr, &reg, 1);
    if (err != ESP_OK) {
        return err;
    }

    reg = (uint8_t)((reg & ~mask) | (value & mask));
    return sx1262_reg_write(addr, &reg, 1);
}

static esp_err_t set_modulation(const sx1262_radio_cfg_t *cfg)
{
    uint8_t bw = 0;
    esp_err_t err = encode_bw(cfg->bw_hz, &bw);
    if (err != ESP_OK) {
        return err;
    }

    const uint8_t args[4] = {
        cfg->sf,
        bw,
        (uint8_t)(cfg->cr - 4),
        needs_ldro(cfg->sf, cfg->bw_hz) ? 0x01 : 0x00,
    };
    err = sx1262_cmd_write(CMD_SET_MODULATION_PARAMS, args, sizeof(args));
    if (err != ESP_OK) {
        return err;
    }

    /* 15.1: without it, frames sent at 500 kHz lose sensitivity at
     * every receiver. Bit 2 clear at 500 kHz, set otherwise. */
    return reg_update(REG_TX_MODULATION, 0x04,
                      cfg->bw_hz == 500000U ? 0x00 : 0x04);
}

static esp_err_t set_packet_params(const sx1262_radio_cfg_t *cfg, uint8_t len)
{
    const uint8_t args[6] = {
        (uint8_t)(cfg->preamble_len >> 8),
        (uint8_t)cfg->preamble_len,
        0x00,                      /* variable length header */
        len,
        cfg->crc_on ? 0x01 : 0x00,
        cfg->iq_inverted ? 0x01 : 0x00,
    };
    return sx1262_cmd_write(CMD_SET_PACKET_PARAMS, args, sizeof(args));
}

/*
 * @param mask      what the chip records in its IRQ status register
 * @param dio1_mask which of those also raise the DIO1 line
 *
 * The two are separate on purpose. A status bit can be read whenever it
 * suits; a DIO1 bit wakes whoever is waiting, and that wake is only
 * useful if there is something safe to do about it right then.
 */
static esp_err_t set_irq_mask_dio(uint16_t mask, uint16_t dio1_mask)
{
    /* IRQ mask, then DIO1/DIO2/DIO3 masks. Only DIO1 is wired out. */
    const uint8_t args[8] = {
        (uint8_t)(mask >> 8), (uint8_t)mask,
        (uint8_t)(dio1_mask >> 8), (uint8_t)dio1_mask,
        0x00, 0x00,
        0x00, 0x00,
    };
    return sx1262_cmd_write(CMD_SET_DIO_IRQ_PARAMS, args, sizeof(args));
}

static esp_err_t set_irq_mask(uint16_t mask)
{
    return set_irq_mask_dio(mask, mask);
}

static esp_err_t get_irq(uint16_t *out)
{
    uint8_t rx[2] = {0};
    esp_err_t err = sx1262_cmd_read_status(CMD_GET_IRQ_STATUS, rx, sizeof(rx));
    if (err == ESP_OK) {
        *out = (uint16_t)((rx[0] << 8) | rx[1]);
    }
    return err;
}

static esp_err_t clear_irq(uint16_t mask)
{
    const uint8_t args[2] = {(uint8_t)(mask >> 8), (uint8_t)mask};
    return sx1262_cmd_write(CMD_CLEAR_IRQ_STATUS, args, sizeof(args));
}

esp_err_t sx1262_radio_configure(const sx1262_radio_cfg_t *cfg)
{
    esp_err_t err = sx1262_radio_check_cfg(cfg);
    if (err != ESP_OK) {
        return err;
    }

    err = set_standby_rc();
    if (err != ESP_OK) {
        return err;
    }

    const uint8_t packet_type = PACKET_TYPE_LORA;
    err = sx1262_cmd_write(CMD_SET_PACKET_TYPE, &packet_type, 1);
    if (err != ESP_OK) {
        return err;
    }

    err = calibrate_image(cfg->freq_hz);
    if (err != ESP_OK) {
        return err;
    }
    err = reg_update(REG_RX_AGC_PATCH, 0x01, 0x01);
    if (err != ESP_OK) {
        return err;
    }
    const uint8_t gain = s_rx_boosted ? RX_GAIN_BOOSTED : RX_GAIN_SAVING;
    err = sx1262_reg_write(REG_RX_GAIN, &gain, 1);
    if (err != ESP_OK) {
        return err;
    }
    err = set_frequency(cfg->freq_hz);
    if (err != ESP_OK) {
        return err;
    }
    err = set_pa(cfg->power_dbm);
    if (err != ESP_OK) {
        return err;
    }
    err = set_modulation(cfg);
    if (err != ESP_OK) {
        return err;
    }
    err = set_packet_params(cfg, 0);
    if (err != ESP_OK) {
        return err;
    }
    /* 15.4: without it, long frames are lost with inverted IQ. Bit 2
     * clear for inverted, set for standard. */
    err = reg_update(REG_IQ_POLARITY, 0x04, cfg->iq_inverted ? 0x00 : 0x04);
    if (err != ESP_OK) {
        return err;
    }

    const uint8_t base[2] = {0x00, 0x00};
    err = sx1262_cmd_write(CMD_SET_BUFFER_BASE_ADDRESS, base, sizeof(base));
    if (err != ESP_OK) {
        return err;
    }

    /*
     * The register takes the sync word spread over two bytes, not the
     * 8-bit value: each nibble sits in the high half of its byte, and
     * the low halves carry control bits that stay at 4. So 0x12 is
     * {0x14, 0x24}.
     */
    const uint8_t sync[2] = {
        (uint8_t)((cfg->sync_word & 0xF0) | 0x04),
        (uint8_t)(((cfg->sync_word & 0x0F) << 4) | 0x04),
    };
    err = sx1262_reg_write(REG_LORA_SYNCWORD, sync, sizeof(sync));
    if (err != ESP_OK) {
        return err;
    }

    s_cfg = *cfg;
    s_configured = true;
    return ESP_OK;
}

esp_err_t sx1262_radio_retune(uint32_t freq_hz)
{
    if (!s_configured) {
        return ESP_ERR_INVALID_STATE;
    }
    if (freq_hz < SX1262_FREQ_MIN_HZ || freq_hz > SX1262_FREQ_MAX_HZ) {
        return ESP_ERR_INVALID_ARG;
    }

    const esp_err_t err = set_frequency(freq_hz);
    if (err != ESP_OK) {
        return err;
    }

    /* Kept in step so get_cfg(), and anything reading it back, describes
     * the frequency actually in the PLL. */
    s_cfg.freq_hz = freq_hz;
    return ESP_OK;
}

esp_err_t sx1262_radio_init(const sx1262_radio_cfg_t *cfg)
{
    esp_err_t err = sx1262_radio_check_cfg(cfg);
    if (err != ESP_OK) {
        return err;
    }

    uint32_t busy_us = 0;
    err = sx1262_reset(&busy_us);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "chip did not come out of reset");
        return err;
    }

    err = set_standby_rc();
    if (err != ESP_OK) {
        return err;
    }

    /* 15.2: the default PA clamp backs off 5-6 dB on a modest antenna
     * mismatch. Lost at reset, so set after every one. */
    err = reg_update(REG_TX_CLAMP, 0x1E, 0x1E);
    if (err != ESP_OK) {
        return err;
    }

    /*
     * The module's TCXO is powered from the chip's own DIO3 and is dead
     * until this runs. Everything frequency-related is meaningless
     * before it, so it comes first and calibration follows it.
     *
     * 0x02 = 1.8 V, and the delay is in 15.625 us steps: 64 gives 1 ms.
     */
    const uint8_t tcxo[4] = {0x02, 0x00, 0x00, 0x40};
    err = sx1262_cmd_write(CMD_SET_DIO3_AS_TCXO_CTRL, tcxo, sizeof(tcxo));
    if (err != ESP_OK) {
        return err;
    }

    /* Calibrate everything now that the reference is running. BUSY
     * stays high through it, about 3.5 ms, and the next command waits
     * that out. */
    const uint8_t cal_all = 0x7F;
    err = sx1262_cmd_write(CMD_CALIBRATE, &cal_all, 1);
    if (err != ESP_OK) {
        return err;
    }

    /* DIO2 drives the antenna switch; RF_SW is held high by R3. */
    const uint8_t enable = 0x01;
    err = sx1262_cmd_write(CMD_SET_DIO2_AS_RF_SWITCH, &enable, 1);
    if (err != ESP_OK) {
        return err;
    }

    return sx1262_radio_configure(cfg);
}

esp_err_t sx1262_radio_standby(void)
{
    return set_standby_rc();
}

/* Bottom of the scale: what the chip returns when it has no value. */
#define RSSI_NO_VALUE_DBM -125

esp_err_t sx1262_radio_rssi_inst(int16_t *out_dbm)
{
    if (!s_configured) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t raw = 0;
    const esp_err_t err =
        sx1262_cmd_read_status(CMD_GET_RSSI_INST, &raw, 1);
    if (err != ESP_OK) {
        return err;
    }

    const int16_t dbm = (int16_t)(-(int)raw / 2);
    if (dbm <= RSSI_NO_VALUE_DBM) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    *out_dbm = dbm;
    return ESP_OK;
}

esp_err_t sx1262_radio_tx_start(const uint8_t *payload, size_t len)
{
    if (!s_configured) {
        return ESP_ERR_INVALID_STATE;
    }
    if (len == 0 || len > SX1262_MAX_PAYLOAD) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = set_standby_rc();
    if (err != ESP_OK) {
        return err;
    }

    err = set_packet_params(&s_cfg, (uint8_t)len);
    if (err != ESP_OK) {
        return err;
    }

    err = clear_irq(0xFFFF);
    if (err != ESP_OK) {
        return err;
    }
    err = set_irq_mask(IRQ_TX_DONE | IRQ_TIMEOUT);
    if (err != ESP_OK) {
        return err;
    }

    /* WriteBuffer takes an offset byte before the payload. */
    uint8_t buf[1 + SX1262_MAX_PAYLOAD];
    buf[0] = 0x00;
    memcpy(&buf[1], payload, len);
    err = sx1262_cmd_write(CMD_WRITE_BUFFER, buf, len + 1);
    if (err != ESP_OK) {
        return err;
    }

    /* Timeout 0 disables the chip's own timeout: the caller times it. */
    const uint8_t tx_args[3] = {0x00, 0x00, 0x00};
    return sx1262_cmd_write(CMD_SET_TX, tx_args, sizeof(tx_args));
}

esp_err_t sx1262_radio_tx_done(void)
{
    if (!s_configured) {
        return ESP_ERR_INVALID_STATE;
    }

    uint16_t irq = 0;
    const esp_err_t err = get_irq(&irq);
    if (err != ESP_OK) {
        return err;
    }

    if ((irq & (IRQ_TX_DONE | IRQ_TIMEOUT)) == 0) {
        return ESP_ERR_NOT_FINISHED;
    }

    clear_irq(0xFFFF);
    return (irq & IRQ_TX_DONE) ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

esp_err_t sx1262_radio_rx_start(void)
{
    if (!s_configured) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = set_standby_rc();
    if (err != ESP_OK) {
        return err;
    }

    err = set_packet_params(&s_cfg, SX1262_MAX_PAYLOAD);
    if (err != ESP_OK) {
        return err;
    }

    err = clear_irq(0xFFFF);
    if (err != ESP_OK) {
        return err;
    }
    /*
     * Every stage on DIO1. The line stays high until every flag mapped
     * to it is cleared, so each flag is cleared as it is read for the
     * next stage to raise its own edge. Clearing mid-reception loses no
     * frames.
     */
    err = set_irq_mask(IRQ_RX_DONE | IRQ_CRC_ERR | IRQ_TIMEOUT
                       | IRQ_PREAMBLE_DET | IRQ_HEADER_VALID
                       | IRQ_HEADER_ERR);
    if (err != ESP_OK) {
        return err;
    }

    /* 0xFFFFFF is continuous mode: the receiver stays on across frames,
     * so nothing is missed while one is being read out. */
    const uint8_t rx_args[3] = {0xFF, 0xFF, 0xFF};
    return sx1262_cmd_write(CMD_SET_RX, rx_args, sizeof(rx_args));
}

/*
 * Frequency error estimate, Hz.
 *
 * Twenty signed bits scaled by the bandwidth. The constant is the one
 * Semtech gives: the raw figure is in units of 1.55 Hz per 1600 kHz of
 * bandwidth, so a wider filter reports the same offset as a smaller
 * number and the bandwidth has to come back in.
 */
static esp_err_t read_freq_error(uint32_t bw_hz, int32_t *out_hz)
{
    uint8_t raw[3] = {0};
    const esp_err_t err = sx1262_reg_read(REG_FREQ_ERROR, raw, sizeof(raw));
    if (err != ESP_OK) {
        return err;
    }

    int32_t efe = ((int32_t)raw[0] << 16) | ((int32_t)raw[1] << 8) | raw[2];
    efe &= 0x0FFFFF;

    /* Sign-extend from twenty bits. */
    if (efe & 0x80000) {
        efe -= 0x100000;
    }

    /*
     * Negated: the chip reports how far it had to pull itself to meet
     * the transmitter, which is the opposite sign from where the
     * transmitter sits. Reported the chip's way round, a transmitter
     * 25 kHz above the tuned channel reads as -25 kHz, and correcting
     * in the direction the number suggests moves away from it.
     */
    *out_hz = -(int32_t)(1.55 * (double)efe * ((double)bw_hz / 1600000.0));
    return ESP_OK;
}

/*
 * Coding rate and CRC presence of the last explicit header received, as
 * RadioLib reads them. Raw 5..7 are the long-interleaver rates of newer
 * chips, taken as 4/5, 4/6, 4/8 the way Meshtastic does. Falls back to
 * the configured values on anything else.
 */
static void read_rx_header(uint8_t *out_cr, bool *out_crc_on)
{
    static const uint8_t CR_FROM_RAW[8] = {0, 5, 6, 7, 8, 5, 6, 8};

    *out_cr = s_cfg.cr;
    *out_crc_on = s_cfg.crc_on;

    uint8_t raw = 0;
    if (sx1262_reg_read(REG_LORA_RX_CR, &raw, 1) == ESP_OK) {
        const uint8_t code = (raw >> 4) & 0x07;
        if (code != 0) {
            *out_cr = CR_FROM_RAW[code];
        }
    }

    if (sx1262_reg_read(REG_FREQ_ERROR, &raw, 1) == ESP_OK) {
        *out_crc_on = (raw & 0x10) != 0;
    }
}

esp_err_t sx1262_radio_rx_header(uint8_t *out_cr, bool *out_crc_on)
{
    if (!s_configured) {
        return ESP_ERR_INVALID_STATE;
    }

    read_rx_header(out_cr, out_crc_on);
    return ESP_OK;
}

esp_err_t sx1262_radio_rx_stages(uint32_t *out_flags)
{
    if (!s_configured) {
        return ESP_ERR_INVALID_STATE;
    }

    uint16_t irq = 0;
    const esp_err_t err = get_irq(&irq);
    if (err != ESP_OK) {
        return err;
    }

    uint32_t flags = 0;
    uint16_t consume = 0;

    if (irq & IRQ_PREAMBLE_DET) {
        flags |= SX1262_STAGE_PREAMBLE;
        consume |= IRQ_PREAMBLE_DET;
    }
    if (irq & IRQ_HEADER_VALID) {
        flags |= SX1262_STAGE_HEADER;
        consume |= IRQ_HEADER_VALID;
    }
    if (irq & IRQ_HEADER_ERR) {
        flags |= SX1262_STAGE_HEADER_ERR;
        consume |= IRQ_HEADER_ERR;
    }

    /*
     * Cleared as they are read, which is what lets the next stage raise
     * DIO1 again: the line only falls once every flag mapped to it is
     * gone. RxDone and CrcErr are left alone - the frame path owns
     * them, and clearing them here would drop frames.
     */
    if (consume != 0) {
        clear_irq(consume);
    }

    *out_flags = flags;
    return ESP_OK;
}

esp_err_t sx1262_radio_rx_collect(sx1262_rx_t *out)
{
    if (!s_configured) {
        return ESP_ERR_INVALID_STATE;
    }

    uint16_t irq = 0;
    esp_err_t err = get_irq(&irq);
    if (err != ESP_OK) {
        return err;
    }

    /*
     * A failed CRC comes with RxDone, and the frame is read out just the
     * same: the measurements beside it are what says why it failed.
     */
    const bool crc_error = (irq & IRQ_CRC_ERR) != 0;

    if ((irq & IRQ_RX_DONE) == 0) {
        /*
         * Flags that raise DIO1 but bring no frame have to be cleared
         * here, or the line stays high and the edge-triggered interrupt
         * behind it never fires again.
         */
        const uint16_t stale = irq & (IRQ_CRC_ERR | IRQ_TIMEOUT);
        if (stale != 0) {
            clear_irq(stale);
        }
        return ESP_ERR_NOT_FOUND;
    }

    err = sx1262_radio_rx_read(out);
    if (err != ESP_OK) {
        return err;
    }
    out->crc_error = crc_error;

    /* Only what this call owns: the stage bits are cleared by
     * rx_stages() as it reads them, which is what lets each of them
     * raise DIO1 in its own right. */
    clear_irq(IRQ_RX_DONE | IRQ_CRC_ERR);
    return crc_error ? ESP_ERR_INVALID_CRC : ESP_OK;
}

esp_err_t sx1262_radio_rx_read(sx1262_rx_t *out)
{
    if (!s_configured) {
        return ESP_ERR_INVALID_STATE;
    }

    memset(out, 0, sizeof(*out));

    uint8_t st[2] = {0};
    esp_err_t err =
        sx1262_cmd_read_status(CMD_GET_RX_BUFFER_STATUS, st, sizeof(st));
    if (err != ESP_OK) {
        return err;
    }

    err = sx1262_buffer_read(st[1], out->payload, st[0]);
    if (err != ESP_OK) {
        return err;
    }
    out->len = st[0];

    uint8_t ps[3] = {0};
    err = sx1262_cmd_read_status(CMD_GET_PACKET_STATUS, ps, sizeof(ps));
    if (err == ESP_OK) {
        out->rssi_dbm = (int16_t)(-(int)ps[0] / 2);
        out->snr_qdb = (int8_t)ps[1];
        out->signal_dbm = (int16_t)(-(int)ps[2] / 2);
    }

    read_freq_error(s_cfg.bw_hz, &out->freq_error_hz);
    read_rx_header(&out->cr, &out->crc_on);
    return ESP_OK;
}

esp_err_t sx1262_radio_irq_take(uint16_t *out_irq)
{
    if (!s_configured) {
        return ESP_ERR_INVALID_STATE;
    }

    uint16_t irq = 0;
    const esp_err_t err = get_irq(&irq);
    if (err != ESP_OK) {
        return err;
    }

    if (irq != 0) {
        clear_irq(irq);
    }

    *out_irq = irq;
    return ESP_OK;
}

/* Datasheet 6.1.4, explicit header. SF5 and SF6 differ: two more
 * preamble symbols, and no 8 in the payload numerator. */
uint32_t sx1262_radio_time_on_air_us(const sx1262_radio_cfg_t *cfg, size_t len)
{
    const uint32_t symbol_us = ((1u << cfg->sf) * 1000000u) / cfg->bw_hz;
    const bool low_sf = cfg->sf <= 6;
    const int de = needs_ldro(cfg->sf, cfg->bw_hz) ? 1 : 0;
    const int crc = cfg->crc_on ? 1 : 0;

    int num = 8 * (int)len - 4 * cfg->sf + 20 + 16 * crc + (low_sf ? 0 : 8);
    int den = 4 * (cfg->sf - 2 * de);
    int payload_symbols = 0;

    if (num > 0) {
        payload_symbols = ((num + den - 1) / den) * cfg->cr;
    }
    payload_symbols += 8;

    const uint32_t preamble_us =
        (cfg->preamble_len + (low_sf ? 6 : 4)) * symbol_us + (symbol_us / 4);
    return preamble_us + (uint32_t)payload_symbols * symbol_us;
}
