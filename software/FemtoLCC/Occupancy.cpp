#include "Occupancy.h"
#include <Arduino.h>

// Oversampling per pass. The ADC is noisy at the level a resistive wheelset
// produces - a couple of counts - so averaging is what makes those detectable.
// 16 reads is about 1.6 ms on the ESP32-C6.
static const uint8_t OVERSAMPLE = 16;

// Calibration averages harder, since the baseline is subtracted from every
// later reading and its error is systematic rather than random.
static const uint16_t CALIBRATE_SAMPLES = 256;

// Exponential smoothing, in sixteenths, applied on top of the oversampling.
static const uint32_t SMOOTH_NUM = 4;    // new sample weight
static const uint32_t SMOOTH_DEN = 16;

// Same constants as Channels::currentMilliamps, kept here so the detector can
// work in ADC counts internally and only convert at the edges.
static const uint32_t ADC_FULL_SCALE_MV = 3300;
static const uint32_t ADC_MAX_COUNT = 4095;
static const uint32_t IPROPI_UA_PER_A = 455;
static const uint32_t SENSE_OHMS = 1430;

static uint32_t countsToMilliamps(uint32_t counts) {
    const uint32_t microvolts = (counts * ADC_FULL_SCALE_MV * 1000u) / ADC_MAX_COUNT;
    return (microvolts * 1000u) / (SENSE_OHMS * IPROPI_UA_PER_A);
}

Occupancy::Occupancy(Channels& channels)
    : channels_(channels), cursor_(0),
      occupiedMa_(5), clearMa_(3), onMs_(20), offMs_(1000) {
    for (int i = 0; i < NUM_CHANNELS; ++i) {
        baseline_[i] = 0;
        filtered_[i] = 0;
        state_[i] = BlockState::Unknown;
        pending_[i] = BlockState::Unknown;
        since_[i] = 0;
        transition_[i] = false;
    }
}

uint32_t Occupancy::readAveraged(uint8_t channel) const {
    uint32_t total = 0;
    for (uint8_t i = 0; i < OVERSAMPLE; ++i) {
        total += analogRead(PIN_SENSE[channel]);
    }
    return total / OVERSAMPLE;
}

void Occupancy::calibrate() {
    for (int i = 0; i < NUM_CHANNELS; ++i) {
        uint32_t total = 0;
        for (uint16_t s = 0; s < CALIBRATE_SAMPLES; ++s) {
            total += analogRead(PIN_SENSE[i]);
        }
        baseline_[i] = (uint16_t)(total / CALIBRATE_SAMPLES);
        filtered_[i] = 0;
    }
}

void Occupancy::setThresholds(uint32_t occupied, uint32_t clear) {
    // Clear must sit below occupied or the hysteresis inverts and the state
    // machine oscillates.
    if (clear >= occupied) {
        clear = occupied > 1 ? occupied - 1 : 0;
    }
    occupiedMa_ = occupied;
    clearMa_ = clear;
}

void Occupancy::setDelays(uint32_t onMs, uint32_t offMs) {
    onMs_ = onMs;
    offMs_ = offMs;
}

void Occupancy::poll() {
    const uint8_t ch = cursor_;
    cursor_ = (uint8_t)((cursor_ + 1) % NUM_CHANNELS);

    // An unpowered block draws nothing, so a reading proves nothing either way.
    if (channels_.mode(ch) == ChannelMode::Off ||
        (channels_.mode(ch) == ChannelMode::DC && channels_.duty(ch) == 0)) {
        if (state_[ch] != BlockState::Unknown) {
            state_[ch] = BlockState::Unknown;
            pending_[ch] = BlockState::Unknown;
            transition_[ch] = true;
        }
        filtered_[ch] = 0;
        return;
    }

    uint32_t raw = readAveraged(ch);
    raw = raw > baseline_[ch] ? raw - baseline_[ch] : 0;   // subtract the tare

    // Smooth in sixteenths to keep the arithmetic integer.
    const uint32_t scaled = raw * SMOOTH_DEN;
    filtered_[ch] = (filtered_[ch] * (SMOOTH_DEN - SMOOTH_NUM) + scaled * SMOOTH_NUM)
                    / SMOOTH_DEN;

    const uint32_t ma = countsToMilliamps(filtered_[ch] / SMOOTH_DEN);

    // Decide what the reading is asking for, with hysteresis between the two
    // thresholds leaving the current state alone.
    BlockState want = state_[ch];
    if (ma >= occupiedMa_) {
        want = BlockState::Occupied;
    } else if (ma < clearMa_) {
        want = BlockState::Clear;
    } else if (state_[ch] == BlockState::Unknown) {
        want = BlockState::Clear;    // energised and below threshold
    }

    const uint32_t now = millis();
    if (want != state_[ch]) {
        if (want != pending_[ch]) {
            pending_[ch] = want;
            since_[ch] = now;
        }
        // Occupancy is asserted quickly but released slowly, so a loco coasting
        // over a dirty patch does not drop the block.
        const uint32_t needed = (want == BlockState::Occupied) ? onMs_ : offMs_;
        if (now - since_[ch] >= needed) {
            state_[ch] = want;
            transition_[ch] = true;
        }
    } else {
        pending_[ch] = want;
        since_[ch] = now;
    }
}

BlockState Occupancy::state(uint8_t channel) const {
    return channel < NUM_CHANNELS ? state_[channel] : BlockState::Unknown;
}

uint32_t Occupancy::milliamps(uint8_t channel) const {
    if (channel >= NUM_CHANNELS) {
        return 0;
    }
    return countsToMilliamps(filtered_[channel] / SMOOTH_DEN);
}

bool Occupancy::takeTransition(uint8_t channel, BlockState& newState) {
    if (channel >= NUM_CHANNELS || !transition_[channel]) {
        return false;
    }
    transition_[channel] = false;
    newState = state_[channel];
    return true;
}
