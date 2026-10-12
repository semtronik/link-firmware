/*
 * Texts of the link-lr screens. One id per text; the French and English
 * are side by side in ui_text.c. Ids ending in _FMT are printf formats.
 */

#pragma once

typedef enum {
    /* Profiles */
    T_PROFILE_1,
    T_PROFILE_2,
    T_PROFILE_3,
    T_RADIO_PROFILE,

    /* Parameters */
    T_REGION,
    T_FREQUENCY,
    T_SPREADING,
    T_BANDWIDTH,
    T_CODING,
    T_SYNC_WORD,
    T_POWER,
    T_PREAMBLE,
    T_RX_GAIN,
    T_OFF,
    T_ON,
    T_IQ_STANDARD,
    T_IQ_INVERTED,
    T_GAIN_SAVING,
    T_GAIN_BOOSTED,
    T_SYMBOLS_SHORT,

    /* Menu and settings */
    T_MENU,
    T_SCAN,
    T_PREFERENCES,
    T_DATE_TIME,
    T_SYSINFO,
    T_ABOUT,
    T_FACTORY_RESET,

    /* Scan */
    T_SCAN_TUNE_FAILED,
    T_SCAN_MEASURING,

    /* System state */
    T_SYS_RADIO,
    T_SYS_HEALTH,
    T_SYS_REC,
    T_RADIO_LISTENING,
    T_RADIO_STOPPED,
    T_REC_STOPPED,
    T_HEALTH_FMT,
    T_REC_LOST_FMT,
    T_REC_FMT,

    /* Capture */
    T_VIEW_JOURNAL,
    T_VIEW_STATS,
    T_FILTER_ALL,
    T_EMPTY_PAYLOAD,
    T_LOG_TAIL_FMT,
    T_NO_FRAME,
    T_YES,
    T_NO,
    T_DATA,
    T_DATA_ONE,
    T_DATA_FMT,
    T_FRAME_INCOMPLETE_LONG,
    T_HEADER_INVALID_LONG,
    T_CRC_FAILED_LONG,
    T_FRAME_DETAILS,
    T_SECTION_MARGIN,
    T_SNR_MEASURED,
    T_SNR_LIMIT_FMT,
    T_MARGIN,
    T_MARGIN_EXPLAIN,
    T_SECTION_CHANNEL,
    T_SIGNAL,
    T_NOISE_BEFORE,
    T_READINGS_FMT,
    T_RANGE_DBM_FMT,
    T_SECTION_FRAME,
    T_FREQ_OFFSET,
    T_CODING_RATE,
    T_TIME_ON_AIR,
    T_PROFILE_DIFFERS_FMT,
    T_RADIO_PROFILE_CAPS,
    T_NONE,
    T_CAPTURE_SETTINGS,
    T_PROTOCOL,
    T_ID_FILTER,
    T_TEST_FRAME,
    T_FRAME_TO_SEND,
    T_CRC_ERROR,
    T_FRAME_INCOMPLETE,
    T_HEADER_INVALID,
    T_SYNC_SHORT,

    /* Stats */
    T_ERRORS,
    T_DURATION_S_FMT,
    T_DURATION_MIN_FMT,
    T_DURATION_H_FMT,
    T_SESSION_FMT,
    T_NO_SESSION,
    T_RECEIVED,
    T_RECEIVED_FMT,
    T_SENT,
    T_SENT_FMT,
    T_ERRORS_BY_TYPE,
    T_TX_DONE,
    T_TX_TIMEOUT,
    T_TX_ABORTED,

    /* First-start setup */
    T_SETUP_REGION_HINT,
    T_SETUP_EU868,
    T_SETUP_US915,
    T_SETUP_PREPARING,

    T_COUNT,
} t_id_t;

/** @brief Text @p id in the current language. */
const char *tr(t_id_t id);
