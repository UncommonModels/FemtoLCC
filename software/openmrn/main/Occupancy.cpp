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

// A pulsed sample arrives once an interval rather than every 40 ms, so it is
// given more weight. 4/16 every 40 ms is about a 160 ms time constant; 8/16
// every 300 ms is about 430 ms, which is as quick as pulsing can be.
static const uint32_t PULSE_SMOOTH_NUM = 8;

// The sense chain needs a moment before a reading means anything: the DRV8874
// takes a few microseconds to turn on, and IPROPI has to charge the 1.43k
// resistor and the ADC input. A sample taken any earlier reads the tare, and
// would hide a light load altogether.
static const uint32_t PULSE_SETTLE_US = 300;

// Defaults, until the configuration is applied. Pulsing is on, because that is
// what makes a dispatcher work out of the box.
static const uint16_t DEFAULT_PULSE_US = 2000;
static const uint16_t DEFAULT_PULSE_INTERVAL_MS = 300;

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
    : channels_(channels), cursor_(0), onMs_(20), offMs_(1000) {
    for (int i = 0; i < NUM_CHANNELS; ++i) {
        baseline_[i] = 0;
        filtered_[i] = 0;
        state_[i] = BlockState::Unknown;
        pending_[i] = BlockState::Unknown;
        since_[i] = 0;
        transition_[i] = false;
        enabled_[i] = true;
        occupiedMa_[i] = 5;
        clearMa_[i] = 3;
        pulseEnabled_[i] = true;
        pulseUs_[i] = DEFAULT_PULSE_US;
        pulseIntervalMs_[i] = DEFAULT_PULSE_INTERVAL_MS;
        lastPulse_[i] = 0;
        pulseFaulted_[i] = false;
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

void Occupancy::setThresholds(uint8_t channel, uint32_t occupied, uint32_t clear) {
    if (channel >= NUM_CHANNELS) {
        return;
    }
    // Clear must sit below occupied or the hysteresis inverts and the state
    // machine oscillates.
    if (clear >= occupied) {
        clear = occupied > 1 ? occupied - 1 : 0;
    }
    occupiedMa_[channel] = occupied;
    clearMa_[channel] = clear;
}

void Occupancy::setThresholds(uint32_t occupied, uint32_t clear) {
    for (uint8_t i = 0; i < NUM_CHANNELS; ++i) {
        setThresholds(i, occupied, clear);
    }
}

void Occupancy::setEnabled(uint8_t channel, bool enabled) {
    if (channel < NUM_CHANNELS) {
        enabled_[channel] = enabled;
    }
}

void Occupancy::setDelays(uint32_t onMs, uint32_t offMs) {
    onMs_ = onMs;
    offMs_ = offMs;
}

void Occupancy::setPulseDetect(uint8_t channel, bool enabled, uint16_t pulseUs,
                               uint16_t intervalMs) {
    if (channel >= NUM_CHANNELS) {
        return;
    }
    pulseEnabled_[channel] = enabled;
    pulseUs_[channel] = pulseUs;
    pulseIntervalMs_[channel] = intervalMs;
    pulseFaulted_[channel] = false;         // new settings, a fresh start

    // Give each channel its own slot in the interval, so the four are pulsed
    // in turn rather than in a burst. Channel 0 waits a whole interval,
    // channel 3 a quarter of one. Unsigned arithmetic wraps exactly, so a
    // start time before zero is still the right distance away.
    lastPulse_[channel] = millis() - (uint32_t)intervalMs * channel / NUM_CHANNELS;
}

bool Occupancy::pulseDetect(uint8_t channel) const {
    return channel < NUM_CHANNELS && enabled_[channel] && pulseEnabled_[channel];
}

bool Occupancy::pulseStopped(uint8_t channel) const {
    return channel < NUM_CHANNELS && pulseFaulted_[channel];
}

// A pulse is only safe, and only means anything, on a block channel that is
// not being driven: never a turnout or an unused output, never one passing the
// DCC signal through - where the multiplexer, not the PWM line, feeds the
// driver - and never one whose driver is reporting a fault.
bool Occupancy::canPulse(uint8_t channel) const {
    return pulseEnabled_[channel] && !pulseFaulted_[channel] &&
           channels_.mode(channel) != ChannelMode::DCC && !channels_.faulted(channel);
}

// Full voltage, in whatever direction the channel was last set to, for as long
// as the pulse lasts. The first few hundred microseconds are thrown away while
// the sense chain settles and the rest is oversampled, which at about 100 us an
// analogRead fits some seventeen reads into the default 2 ms - as many as a
// powered channel's pass gets.
uint32_t Occupancy::pulseAndRead(uint8_t channel) {
    const uint32_t start = micros();
    const uint32_t settled = start + PULSE_SETTLE_US;
    const uint32_t deadline = start + pulseUs_[channel];

    channels_.probe(channel, true);
    while ((int32_t)(micros() - settled) < 0) {
    }

    uint32_t total = 0;
    uint32_t samples = 0;
    do {
        total += analogRead(PIN_SENSE[channel]);
        ++samples;
    } while ((int32_t)(micros() - deadline) < 0);
    channels_.probe(channel, false);

    const uint32_t raw = total / samples;
    return raw > baseline_[channel] ? raw - baseline_[channel] : 0;   // subtract the tare
}

bool Occupancy::pulseOnce(uint8_t channel, uint32_t& milliamps) {
    if (channel >= NUM_CHANNELS || channels_.mode(channel) == ChannelMode::DCC ||
        channels_.duty(channel) != 0 || channels_.faulted(channel)) {
        return false;
    }
    lastPulse_[channel] = millis();         // not twice in a row
    milliamps = countsToMilliamps(pulseAndRead(channel));
    // Asking for a pulse by hand is also how a channel is tried again after a
    // short tripped its driver.
    pulseFaulted_[channel] = channels_.faulted(channel);
    return true;
}

void Occupancy::setUnknown(uint8_t channel) {
    if (state_[channel] != BlockState::Unknown) {
        state_[channel] = BlockState::Unknown;
        pending_[channel] = BlockState::Unknown;
        transition_[channel] = true;
    }
    filtered_[channel] = 0;
}

void Occupancy::poll() {
    const uint8_t ch = cursor_;
    cursor_ = (uint8_t)((cursor_ + 1) % NUM_CHANNELS);

    // A turnout motor's current is not occupancy, and neither is an unused
    // output's.
    if (!enabled_[ch]) {
        setUnknown(ch);
        return;
    }

    const ChannelMode mode = channels_.mode(ch);
    const bool energised = mode == ChannelMode::DCC ||
                           (mode == ChannelMode::DC && channels_.duty(ch) != 0);
    const uint32_t now = millis();
    uint32_t raw;
    bool pulsed = false;

    if (energised) {
        pulseFaulted_[ch] = false;      // driving the channel clears the latch
        raw = readAveraged(ch);
        raw = raw > baseline_[ch] ? raw - baseline_[ch] : 0;   // subtract the tare
    } else if (!canPulse(ch)) {
        // Nothing flowing and nothing we may do about it: a reading would
        // prove nothing either way.
        setUnknown(ch);
        return;
    } else if (now - lastPulse_[ch] < pulseIntervalMs_[ch]) {
        return;                         // between pulses; the state stands
    } else {
        lastPulse_[ch] = now;
        raw = pulseAndRead(ch);
        pulsed = true;
        // A driver that trips on the pulse - a shorted block, most likely -
        // is not pulsed again, or it would fault every interval for as long as
        // the short is there. Driving the channel clears it, as does a pulse
        // asked for from the console.
        if (channels_.faulted(ch)) {
            pulseFaulted_[ch] = true;
            setUnknown(ch);
            return;
        }
    }

    // Smooth in sixteenths to keep the arithmetic integer.
    const uint32_t weight = pulsed ? PULSE_SMOOTH_NUM : SMOOTH_NUM;
    const uint32_t scaled = raw * SMOOTH_DEN;
    filtered_[ch] = (filtered_[ch] * (SMOOTH_DEN - weight) + scaled * weight)
                    / SMOOTH_DEN;

    const uint32_t ma = countsToMilliamps(filtered_[ch] / SMOOTH_DEN);

    // Decide what the reading is asking for, with hysteresis between the two
    // thresholds leaving the current state alone.
    BlockState want = state_[ch];
    if (ma >= occupiedMa_[ch]) {
        want = BlockState::Occupied;
    } else if (ma < clearMa_[ch]) {
        want = BlockState::Clear;
    } else if (state_[ch] == BlockState::Unknown) {
        want = BlockState::Clear;    // measurable, and below threshold
    }

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
