/**
 * @file link_scan.h
 * @brief Energy sweep over the band, and the snapshot the screen reads.
 *
 * Answers the one question that needs no radio settings at all: is
 * anything transmitting, and where. Instantaneous RSSI does not know
 * what LoRa is - it measures power in the filter, whatever the
 * modulation - so neither the spreading factor, the coding rate nor the
 * sync word takes part. Only the frequency does, which is why this
 * looks for that first and leaves the rest to the monitor.
 *
 * The sweep owns the radio while it runs: it retunes constantly, so
 * nothing can be received at the same time. link_radio_suspend() is
 * what makes room for it.
 *
 * Screens never call the driver. They take a snapshot, which is
 * consistent by construction: one lock, one struct copy.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * The band swept, Hz.
 *
 * Swept continuously, channel after channel, rather than parked on one
 * and moved by hand. What makes that work is how little a channel
 * costs: retuning is a PLL write, reading the RSSI is a register read,
 * so a channel is a millisecond or two and the whole band is swept tens
 * of times a second. A transmission lasts longer than one pass - the
 * shortest LoRa frame worth the name runs tens of milliseconds - so it
 * cannot fall between the cracks the way it would if a pass took a
 * second.
 *
 * An earlier attempt at sweeping did fail this way, and the difference
 * is worth keeping in mind: it retuned through the full configure path,
 * image calibration and all, which costs milliseconds per channel and
 * stretched a pass past the length of a frame.
 */
#define LINK_SCAN_FROM_HZ 863000000U
#define LINK_SCAN_TO_HZ   870000000U

/**
 * Measurement apertures, widest first - which is also the order to use
 * them in: wide to find, narrow to pin down.
 *
 * The channel spacing always equals the filter width, so consecutive
 * channels tile the band without a hole whichever one is selected. It
 * says nothing about the bandwidth the transmitter uses: this is a
 * measuring aperture, not a demodulator.
 *
 * Wide is the default because it is the only one whose pass is short
 * enough to catch any LoRa frame: fifteen channels sweep in about
 * 25 ms, under even a short SF7 frame, where the narrowest takes four
 * times as long and starts missing them. Fifteen bars also read at a
 * glance where fifty-seven are a few pixels each.
 *
 * Narrowing then buys two things on the channel found: two emitters
 * 200 kHz apart separate instead of landing in one filter, and less
 * noise is taken in, so the floor drops and a weak signal stands
 * clearer of it.
 */
typedef enum {
    LINK_SCAN_BW_500K,
    LINK_SCAN_BW_250K,
    LINK_SCAN_BW_125K,
    LINK_SCAN_BW_COUNT,
} link_scan_bw_t;

/** @brief Aperture width in Hz, which is also the channel spacing. */
uint32_t link_scan_bw_hz(link_scan_bw_t bw);

/** @brief Channels at this aperture, one per step, both ends included. */
uint16_t link_scan_channels(link_scan_bw_t bw);

/**
 * Channels at the narrowest aperture - the most there can ever be, and
 * so what the per-channel arrays are sized for.
 */
#define LINK_SCAN_MAX_CHANNELS \
    (((LINK_SCAN_TO_HZ - LINK_SCAN_FROM_HZ) / 125000U) + 1)

/**
 * Ends of the displayed scale, dBm.
 *
 * Not arbitrary: the driver treats anything at or below -125 dBm as the
 * chip reporting no value at all, which is the bottom of what can be
 * measured, and the top of the receiver's range is 0 dBm. -20 is where
 * the top is cut instead, because a signal that strong is a transmitter
 * in the same room and the detail worth seeing is all below it.
 */
#define LINK_SCAN_DBM_MIN (-125)
#define LINK_SCAN_DBM_MAX (-20)

/**
 * Level a reading must reach to count as activity, dBm.
 *
 * Absolute rather than a margin over each channel's own floor. A margin
 * adapts to the room, but it cannot be drawn: it has no value in dBm,
 * so there is no line to put on the chart and nothing to compare a bar
 * against by eye. A fixed level can be drawn once, across the whole
 * profile, and read directly.
 *
 * -100 dBm sits well above a quiet indoor noise floor and well below
 * anything transmitting nearby.
 */
#define LINK_SCAN_HIT_DBM (-100)

/**
 * Passes over the band between one dB of floor decay.
 *
 * The floor has to rise as well as fall. A pure min hold only ever
 * descends, so it settles on the deepest trough the noise has ever
 * reached and stays there - after which ordinary noise stands a
 * permanent ten dB above it, every reading scores as a burst, and every
 * channel's floor ends up at its own extreme trough rather than at its
 * noise, so the profile bars all come out the same height.
 *
 * Decay fixes both: a floor drops to a trough at once and creeps back
 * up while none arrives, tracking the noise instead of its record low.
 * Slow enough that a transmission - which pushes readings up, never
 * down - cannot drag it along.
 */
#define LINK_SCAN_FLOOR_DECAY_PASSES 20

/**
 * Passes between one dB of peak decay.
 *
 * Peaks decay for the same reason floors do, and much more slowly. A
 * plain max hold catches a burst and keeps it, which is what lets the
 * operator trigger the device and only then look - but held forever it
 * equally keeps the loudest crest of the noise, and every channel has
 * one, so every bar ends up the same height above its floor.
 *
 * Slow enough that a burst stays legible long after the trigger, quick
 * enough that a noise crest fades in well under a minute.
 */
#define LINK_SCAN_PEAK_DECAY_PASSES 150

typedef struct {
    /** False when the sweep is not running. */
    bool running;
    /** True once the tuner failed - what an empty reading means. */
    bool tune_failed;
    /** Measurement aperture in use, and its width in Hz. */
    link_scan_bw_t bw;
    uint32_t bw_hz;
    /** Channels at that aperture - how much of the array is in use. */
    uint16_t channels;
    /** Passes completed over the band since the last reset. */
    uint32_t passes;
    /**
     * Per-channel traces, dBm, indexed by channel.
     *
     * Max hold is what catches a transmission: a burst leaves its mark
     * and stays, so the operator can trigger the device and then look,
     * rather than having to watch at the right instant. The floor is a
     * min hold that decays upward, per LINK_SCAN_FLOOR_DECAY_PASSES.
     *
     * A channel is only meaningful once measured; before that the pair
     * is unset rather than zero.
     */
    int16_t chan_max[LINK_SCAN_MAX_CHANNELS];
    int16_t chan_min[LINK_SCAN_MAX_CHANNELS];
    bool chan_measured[LINK_SCAN_MAX_CHANNELS];
    /**
     * Readings that stood clear of their channel's floor, and when the
     * last one was. Kept alongside the traces because the traces say
     * how strong, never how recently - and what identifies the device
     * under test is a burst arriving as it is triggered.
     */
    uint32_t hits;
    int64_t last_hit_ms;
    /** Strongest channel, and whether there is one at all. */
    bool have_peak;
    uint16_t peak_channel;
} link_scan_state_t;

/**
 * @brief Start sweeping.
 *
 * The caller must have suspended reception first: this drives the
 * tuner and the two cannot share it.
 */
esp_err_t link_scan_start(void);

/** @brief Stop sweeping and wait for the task to leave the radio alone. */
void link_scan_stop(void);

/** @brief Forget every measurement and start the profile over. */
void link_scan_reset(void);

/**
 * @brief Switch to the next aperture, widening and wrapping round.
 *
 * Measurements are cleared: the channels are renumbered, and readings
 * taken through a different filter width are not comparable to the new
 * ones anyway - a wider aperture takes in more noise, lifting the floor
 * along with everything measured against it.
 */
void link_scan_cycle_bw(void);

/** @brief Centre frequency of a channel at the current aperture, Hz. */
uint32_t link_scan_channel_hz(link_scan_bw_t bw, uint16_t channel);

/** @brief Consistent copy of the current state. */
void link_scan_get(link_scan_state_t *out);

#ifdef __cplusplus
}
#endif
