/**
 * @file link_stats.h
 * @brief Counts of the recording session.
 *
 * Fed by the recorder as it writes each line, so the counts are those of
 * the file, and restarted when a session opens. Read from any task.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "link_radio.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /** False until a session has opened since boot. */
    bool ready;
    uint32_t session;
    /** Boot ms at the opening. */
    int64_t since_ms;

    /** Read without error, and the same by margin class. */
    uint32_t ok;
    uint32_t good;
    uint32_t fair;
    uint32_t poor;

    uint32_t crc;
    uint32_t hdr;
    uint32_t incomplete;

    uint32_t tx_ok;
    uint32_t tx_timeout;
    uint32_t tx_aborted;
} link_stats_t;

/** A session opens: counts restart. */
void link_stats_begin(uint32_t session);

void link_stats_note_rx(const link_radio_entry_t *e);
void link_stats_note_tx(const link_radio_tx_t *tx);

void link_stats_get(link_stats_t *out);

#ifdef __cplusplus
}
#endif
