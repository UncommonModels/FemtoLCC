// Block occupancy detection.
//
// Anything drawing current in a block - a locomotive, a lit car, a resistive
// wheelset - shows up on the DRV8874's IPROPI output, which each channel already
// feeds to an ADC pin. This turns that into a debounced occupied/clear state.
//
// Sensitivity, measured from the sense chain: IPROPI is 455 uA/A into the
// 1.43 k resistors at R7/R9/R11/R13, giving 0.65 V per amp. Against a 12-bit
// 3.3 V ADC that is 1.24 mA per count, so:
//
//   running locomotive      ~300 mA   242 counts   unmistakable
//   idle DCC locomotive      ~60 mA    48 counts   reliable
//   lit passenger car        ~20 mA    16 counts   reliable
//   4.7k resistive wheelset  ~3 mA      2.4 counts marginal
//   10k resistive wheelset   ~1.4 mA    1.1 counts below the noise floor
//
// Oversampling and a per-channel baseline tare are what make the bottom two rows
// workable at all. Use 4.7k or lower wheelsets; 10k will not detect reliably.
//
// Detection only means anything while the block is energised. A channel that is
// Off carries no current, so its state is reported as Unknown rather than Clear.

#pragma once

#include <stdint.h>
#include "board.h"
#include "Channels.h"

enum class BlockState : uint8_t {
    Unknown,    // block not energised, nothing can be inferred
    Clear,
    Occupied
};

class Occupancy {
public:
    explicit Occupancy(Channels& channels);

    // Measures each channel's zero-current reading so it can be subtracted.
    // Call with every channel off, at startup, before any track power.
    void calibrate();

    // Sample one channel per call, round-robin. Call every few milliseconds.
    void poll();

    BlockState state(uint8_t channel) const;
    uint32_t milliamps(uint8_t channel) const;

    // True exactly once per transition, so the caller can fire an LCC event.
    bool takeTransition(uint8_t channel, BlockState& newState);

    // Thresholds in milliamps. Occupied must exceed `occupied`; the block is not
    // released until it falls below `clear`. The gap is hysteresis and stops a
    // block on the threshold from chattering.
    void setThresholds(uint32_t occupied, uint32_t clear);

    // A block is declared occupied after `onMs` above threshold, and released
    // only after `offMs` below it. The release delay is the important one: it
    // rides over dirty track and the gaps between wheelsets.
    void setDelays(uint32_t onMs, uint32_t offMs);

private:
    uint32_t readAveraged(uint8_t channel) const;

    Channels& channels_;
    uint8_t cursor_;                        // channel being sampled this pass

    uint16_t baseline_[NUM_CHANNELS];       // zero-current ADC reading
    uint32_t filtered_[NUM_CHANNELS];       // smoothed reading, ADC counts x16
    BlockState state_[NUM_CHANNELS];
    BlockState pending_[NUM_CHANNELS];
    uint32_t since_[NUM_CHANNELS];          // when the pending state began
    bool transition_[NUM_CHANNELS];

    uint32_t occupiedMa_;
    uint32_t clearMa_;
    uint32_t onMs_;
    uint32_t offMs_;
};
