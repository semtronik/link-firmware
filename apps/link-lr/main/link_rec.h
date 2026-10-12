/**
 * @file link_rec.h
 * @brief The recording: every reception and transmission, kept on flash.
 *
 * Format and rules: link/protocol/v1/recording.md.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /** False while no session is open, e.g. on an unset clock. */
    bool open;
    uint32_t session;
    /** Lines written in this session. */
    uint32_t lines;
    /** Lines lost since boot; must stay at 0. */
    uint32_t lost;
    /** All recordings on flash, bytes. */
    uint32_t total_bytes;
} link_rec_stats_t;

/**
 * @brief Start recording from the radio.
 *
 * After link_events_init(), which mounts the partition, and after
 * link_profile_init(): the session line names the profile.
 */
esp_err_t link_rec_start(void);

void link_rec_get_stats(link_rec_stats_t *out);

/**
 * @brief Let a session open, if none is running and the clock is set.
 *
 * Called by the Journal screen while it is displayed: sessions open there
 * and nowhere else.
 */
void link_rec_allow(void);

/**
 * @brief Close the running session and wait until it is on flash.
 *
 * Waits at most a second. Nothing reopens until link_rec_allow().
 *
 * @param reason As recording.md lists them. Must outlive the session: a
 *               string literal.
 */
void link_rec_close(const char *reason);

/** A session on flash: its parts run from first_part to last_part. */
typedef struct {
    uint32_t id;
    unsigned first_part;
    unsigned last_part;
    uint32_t bytes;
} link_rec_session_t;

/** @brief The sessions on flash, oldest first. Returns how many. */
int link_rec_list(link_rec_session_t *out, int max);

/** @brief The first and last complete lines of a part, newline stripped. */
esp_err_t link_rec_edge_lines(uint32_t session, unsigned part, char *first,
                              char *last, size_t size);

typedef void (*link_rec_line_cb_t)(const char *line, void *user_data);

/**
 * @brief Each line of a part between two byte offsets, newline stripped.
 *
 * @param to UINT32_MAX for the end of the file.
 * @return Lines read, or -1 if the part is not there.
 */
int link_rec_read(uint32_t session, unsigned part, uint32_t from,
                  uint32_t to, link_rec_line_cb_t cb, void *user_data);

/** Where the recording stood when live lines were held. */
typedef struct {
    uint32_t session;
    unsigned part;
    uint32_t offset;
} link_rec_mark_t;

/**
 * @brief Stop sending live lines; they keep being recorded.
 *
 * Everything before the mark is on flash when this returns. Pair with
 * link_rec_release(), always.
 */
void link_rec_hold(link_rec_mark_t *out);

/** @brief Send what was recorded since the mark, then go live again. */
void link_rec_release(void);

/** @brief Close the session and delete every recording. */
esp_err_t link_rec_erase(void);

#ifdef __cplusplus
}
#endif
