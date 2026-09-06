#include "Channels.h"
#include <Arduino.h>

// LEDC PWM. 16 kHz keeps switching above the audible range so a block does not
// whine, while staying slow enough for the DRV8874's slew rate.
static const uint32_t PWM_FREQ_HZ = 16000;
static const uint8_t PWM_RESOLUTION_BITS = 8;

// DRV8874 IPROPI: 455 uA out per amp through the bridge, across the 1.43k
// resistor at R7/R9/R11/R13. 1 A therefore reads 455 uA * 1430 R = 0.65 V.
static const uint32_t IPROPI_UA_PER_A = 455;
static const uint32_t SENSE_OHMS = 1430;
static const uint32_t ADC_FULL_SCALE_MV = 3300;
static const uint32_t ADC_MAX_COUNT = 4095;

Channels::Channels(Expander& expander) : expander_(expander) {
    for (int i = 0; i < NUM_CHANNELS; ++i) {
        mode_[i] = ChannelMode::Off;
        reverse_[i] = false;
        duty_[i] = 0;
    }
}

void Channels::begin() {
    for (int i = 0; i < NUM_CHANNELS; ++i) {
        ledcAttach(PIN_PWM[i], PWM_FREQ_HZ, PWM_RESOLUTION_BITS);
        ledcWrite(PIN_PWM[i], 0);

        pinMode(PIN_NFAULT[i], INPUT_PULLUP);   // open drain, active low
        pinMode(PIN_SENSE[i], INPUT);

        if (PIN_DIR[i] >= 0) {
            pinMode(PIN_DIR[i], OUTPUT);
            digitalWrite(PIN_DIR[i], LOW);
        }
    }
    allOff();
    flush();
}

void Channels::set(uint8_t channel, ChannelMode mode, bool reverse, uint8_t duty) {
    if (channel >= NUM_CHANNELS) {
        return;
    }
    setReverse(channel, reverse);
    setMode(channel, mode);
    setDuty(channel, duty);
}

void Channels::setMode(uint8_t channel, ChannelMode mode) {
    if (channel >= NUM_CHANNELS) {
        return;
    }
    mode_[channel] = mode;
    applyMode(channel, mode);

    // In DCC mode the multiplexer feeds the track signal straight through, so
    // the PWM line must be idle or it fights the mux inputs.
    if (mode != ChannelMode::DC) {
        ledcWrite(PIN_PWM[channel], 0);
    } else {
        ledcWrite(PIN_PWM[channel], duty_[channel]);
    }
}

void Channels::setReverse(uint8_t channel, bool reverse) {
    if (channel >= NUM_CHANNELS) {
        return;
    }
    reverse_[channel] = reverse;
    applyDir(channel, reverse);
}

void Channels::setDuty(uint8_t channel, uint8_t duty) {
    if (channel >= NUM_CHANNELS) {
        return;
    }
    duty_[channel] = duty;
    if (mode_[channel] == ChannelMode::DC) {
        ledcWrite(PIN_PWM[channel], duty);
    }
}

void Channels::allOff() {
    for (int i = 0; i < NUM_CHANNELS; ++i) {
        setMode(i, ChannelMode::Off);
        setDuty(i, 0);
    }
}

void Channels::applyMode(uint8_t channel, ChannelMode mode) {
    // dcc_en high routes the multiplexer to the DCC inputs. Off and DC both
    // leave it low; Off is expressed by holding the PWM at zero duty.
    expander_.setPortBBit(EXP_DCC_EN_BIT[channel], mode == ChannelMode::DCC);
}

void Channels::applyDir(uint8_t channel, bool reverse) {
    if (PIN_DIR[channel] >= 0) {
        digitalWrite(PIN_DIR[channel], reverse ? HIGH : LOW);
    } else {
        expander_.setPortBBit(EXP_DIR_BIT[channel], reverse);
    }
}

ChannelMode Channels::mode(uint8_t channel) const {
    return channel < NUM_CHANNELS ? mode_[channel] : ChannelMode::Off;
}

bool Channels::reversed(uint8_t channel) const {
    return channel < NUM_CHANNELS && reverse_[channel];
}

uint8_t Channels::duty(uint8_t channel) const {
    return channel < NUM_CHANNELS ? duty_[channel] : 0;
}

bool Channels::faulted(uint8_t channel) const {
    if (channel >= NUM_CHANNELS) {
        return false;
    }
    return digitalRead(PIN_NFAULT[channel]) == LOW;
}

uint32_t Channels::currentMilliamps(uint8_t channel) const {
    if (channel >= NUM_CHANNELS) {
        return 0;
    }
    const uint32_t counts = analogRead(PIN_SENSE[channel]);
    const uint32_t microvolts = (counts * ADC_FULL_SCALE_MV * 1000u) / ADC_MAX_COUNT;
    // I(A) = V(uV) / (SENSE_OHMS * IPROPI_UA_PER_A) ... in milliamps:
    return (microvolts * 1000u) / (SENSE_OHMS * IPROPI_UA_PER_A);
}

bool Channels::flush() {
    return expander_.flush();
}
