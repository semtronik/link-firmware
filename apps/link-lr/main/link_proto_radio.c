/*
 * The radio half of the JSON protocol. The platform commands - time,
 * version, events, OTA, reboot - live in link-core-app; these are
 * registered on top of them at start-up.
 */

#include "link_proto_radio.h"

#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "link_profile.h"
#include "link_proto.h"
#include "link_radio.h"
#include "link_rec.h"
#include "link_regul.h"

static const char *TAG = "proto_radio";

/* The ack, with the selected profile as the session line carries it. */
static void send_profile_ack(const char *cmd)
{
    char text[LINK_PROFILE_JSON_MAX];
    if (link_profile_current_json(text, sizeof(text)) == 0) {
        link_proto_send_err(cmd, "profile too long to send");
        return;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "ack");
    cJSON_AddStringToObject(root, "cmd", cmd);
    cJSON_AddStringToObject(root, "status", "ok");
    cJSON_AddItemToObject(root, "profile", cJSON_Parse(text));
    link_proto_send_json(root);
}

static void handle_get_profile(const cJSON *root, const cJSON *value)
{
    (void)root;
    (void)value;
    send_profile_ack("get_profile");
}

/* A whole number in [min, max]: 9.5 is no spreading factor. */
static bool as_int(const cJSON *item, double min, double max, long *out)
{
    if (!cJSON_IsNumber(item)) {
        return false;
    }
    const double v = item->valuedouble;
    if (v < min || v > max || v != (double)(long)v) {
        return false;
    }
    *out = (long)v;
    return true;
}

/* One field of set_profile into cfg. False with msg set when it is not
 * one, or not a legal value. */
static bool parse_field(const cJSON *item, link_profile_cfg_t *cfg, char *msg,
                        size_t size)
{
    const char *key = item->string;
    sx1262_radio_cfg_t *radio = &cfg->radio;
    long v = 0;

    if (strcmp(key, "region") == 0) {
        const link_region_t region =
            cJSON_IsString(item) ? link_regul_region_from_name(item->valuestring)
                                 : LINK_REGION_COUNT;
        if (region == LINK_REGION_COUNT) {
            snprintf(msg, size, "region must be EU868 or US915");
            return false;
        }
        cfg->region = region;
    } else if (strcmp(key, "freq_hz") == 0) {
        if (!as_int(item, SX1262_FREQ_MIN_HZ, SX1262_FREQ_MAX_HZ, &v)) {
            snprintf(msg, size, "freq_hz must be %u to %u",
                     (unsigned)SX1262_FREQ_MIN_HZ, (unsigned)SX1262_FREQ_MAX_HZ);
            return false;
        }
        radio->freq_hz = (uint32_t)v;
    } else if (strcmp(key, "sf") == 0) {
        if (!as_int(item, 5, 12, &v)) {
            snprintf(msg, size, "sf must be 5 to 12");
            return false;
        }
        radio->sf = (uint8_t)v;
    } else if (strcmp(key, "bw_hz") == 0) {
        /* The config screen's four, not every step the chip has: a
         * profile set here must be one the device can set back. */
        if (!as_int(item, 1, 500000, &v)
            || (v != 62500 && v != 125000 && v != 250000 && v != 500000)) {
            snprintf(msg, size, "bw_hz must be 62500, 125000, 250000 or 500000");
            return false;
        }
        radio->bw_hz = (uint32_t)v;
    } else if (strcmp(key, "cr") == 0) {
        if (!as_int(item, 5, 8, &v)) {
            snprintf(msg, size, "cr must be 5 to 8");
            return false;
        }
        radio->cr = (uint8_t)v;
    } else if (strcmp(key, "power_dbm") == 0) {
        if (!as_int(item, -9, 22, &v)) {
            snprintf(msg, size, "power_dbm must be -9 to 22");
            return false;
        }
        radio->power_dbm = (int8_t)v;
    } else if (strcmp(key, "preamble") == 0) {
        if (!as_int(item, 1, 65535, &v)) {
            snprintf(msg, size, "preamble must be 1 to 65535");
            return false;
        }
        radio->preamble_len = (uint16_t)v;
    } else if (strcmp(key, "crc_on") == 0) {
        if (!cJSON_IsBool(item)) {
            snprintf(msg, size, "crc_on must be true or false");
            return false;
        }
        radio->crc_on = cJSON_IsTrue(item);
    } else if (strcmp(key, "iq_inverted") == 0) {
        if (!cJSON_IsBool(item)) {
            snprintf(msg, size, "iq_inverted must be true or false");
            return false;
        }
        radio->iq_inverted = cJSON_IsTrue(item);
    } else if (strcmp(key, "sync_word") == 0) {
        if (!as_int(item, 0, 255, &v)) {
            snprintf(msg, size, "sync_word must be 0 to 255");
            return false;
        }
        radio->sync_word = (uint8_t)v;
    } else if (strcmp(key, "rx_gain") == 0) {
        const char *name = cJSON_IsString(item) ? item->valuestring : "";
        if (strcmp(name, "boosted") != 0 && strcmp(name, "saving") != 0) {
            snprintf(msg, size, "rx_gain must be saving or boosted");
            return false;
        }
        cfg->rx_boosted = strcmp(name, "boosted") == 0;
    } else if (strcmp(key, "name") == 0) {
        snprintf(msg, size, "name cannot be set");
        return false;
    } else {
        snprintf(msg, size, "unknown field: %.40s", key);
        return false;
    }

    return true;
}

/*
 * Every field checked before anything is touched: one bad value leaves
 * the radio, the slot and the session as they were. Fields left out keep
 * their value.
 */
static void handle_set_profile(const cJSON *root, const cJSON *value)
{
    (void)value;
    const cJSON *fields = cJSON_GetObjectItem(root, "profile");
    if (!cJSON_IsObject(fields)) {
        link_proto_send_err("set_profile", "profile must be an object");
        return;
    }

    link_profile_cfg_t cfg;
    link_profile_get_current(&cfg);

    char msg[80];
    const cJSON *item;
    cJSON_ArrayForEach(item, fields) {
        if (!parse_field(item, &cfg, msg, sizeof(msg))) {
            link_proto_send_err("set_profile", msg);
            return;
        }
    }

    /* Each field is checked on its own; a bandwidth left out may still be
     * one of the narrow steps, from a profile stored before. */
    if (sx1262_radio_check_cfg(&cfg.radio) != ESP_OK) {
        link_proto_send_err("set_profile", "radio settings out of range");
        return;
    }

    link_rec_close("app");
    const esp_err_t err = link_profile_update_current(&cfg);

    if (err == ESP_ERR_INVALID_STATE) {
        link_proto_send_err("set_profile", "no radio module");
    } else if (err != ESP_OK) {
        link_proto_send_err("set_profile", "radio refused the settings");
    } else {
        send_profile_ack("set_profile");
    }
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

/* Answered as soon as the frame is on air; how it ended follows as a
 * "tx" line, so the protocol task never waits out a transmission. */
static void handle_tx(const cJSON *root, const cJSON *value)
{
    (void)root;
    if (!cJSON_IsString(value)) {
        link_proto_send_err("tx", "value must be a hex string");
        return;
    }

    const char *hex = value->valuestring;
    const size_t chars = strlen(hex);
    if (chars == 0 || chars % 2 != 0 || chars > SX1262_MAX_PAYLOAD * 2) {
        link_proto_send_err("tx", "value must be 1 to 255 bytes of hex");
        return;
    }

    uint8_t payload[SX1262_MAX_PAYLOAD];
    const size_t len = chars / 2;
    for (size_t i = 0; i < len; i++) {
        const int hi = hex_digit(hex[2 * i]);
        const int lo = hex_digit(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            link_proto_send_err("tx", "value must be 1 to 255 bytes of hex");
            return;
        }
        payload[i] = (uint8_t)((hi << 4) | lo);
    }

    const esp_err_t err = link_radio_tx_submit(payload, len);

    if (err == ESP_OK) {
        link_proto_send_ack("tx");
    } else if (err == ESP_ERR_INVALID_STATE) {
        link_proto_send_err("tx", "no radio module");
    } else if (err == ESP_ERR_NOT_ALLOWED) {
        link_proto_send_err("tx", "radio busy (scan)");
    } else if (err == ESP_ERR_NOT_FINISHED) {
        link_proto_send_err("tx", "previous frame still on air");
    } else {
        link_proto_send_err("tx", "radio refused the frame");
    }
}

esp_err_t link_proto_radio_register(void)
{
    ESP_RETURN_ON_ERROR(link_proto_register("get_profile", handle_get_profile),
                        TAG, "get_profile");
    ESP_RETURN_ON_ERROR(link_proto_register("set_profile", handle_set_profile),
                        TAG, "set_profile");
    ESP_RETURN_ON_ERROR(link_proto_register("tx", handle_tx), TAG, "tx");

    return ESP_OK;
}
