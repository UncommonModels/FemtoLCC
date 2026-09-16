#include "Turnouts.h"
#include <Arduino.h>

Turnouts::Turnouts(Channels& channels) : channels_(channels) {
    for (int i = 0; i < NUM_CHANNELS; ++i) {
        motor_[i] = Motor{ false, MotorType::Stall, 0, 0, false,
                           TurnoutPosition::Unknown, false, 0 };
    }
}

void Turnouts::attach(uint8_t channel, const ChannelSettings& settings) {
    if (channel >= NUM_CHANNELS) {
        return;
    }
    Motor& m = motor_[channel];
    const bool wasAttached = m.attached;
    m.attached = true;
    m.type = settings.motor;
    m.pulseMs = settings.pulseMs;
    m.duty = settings.duty;
    m.reverse = settings.reverse;

    if (!wasAttached) {
        m.position = TurnoutPosition::Unknown;
        m.pulsing = false;
        return;
    }
    // Settings changed under a turnout that is already in use.
    if (m.type == MotorType::Stall && m.position != TurnoutPosition::Unknown) {
        m.pulsing = false;
        drive(channel);
    } else if (m.type == MotorType::Pulse && !m.pulsing) {
        channels_.set(channel, ChannelMode::Off, false, 0);
        channels_.flush();
    }
}

void Turnouts::detach(uint8_t channel) {
    if (channel >= NUM_CHANNELS) {
        return;
    }
    motor_[channel].attached = false;
    motor_[channel].pulsing = false;
    motor_[channel].position = TurnoutPosition::Unknown;
}

bool Turnouts::attached(uint8_t channel) const {
    return channel < NUM_CHANNELS && motor_[channel].attached;
}

void Turnouts::set(uint8_t channel, bool thrown) {
    if (!attached(channel)) {
        return;
    }
    motor_[channel].position = thrown ? TurnoutPosition::Thrown : TurnoutPosition::Closed;
    drive(channel);
}

void Turnouts::drive(uint8_t channel) {
    Motor& m = motor_[channel];
    // Closed is forward, thrown reverse; the reverse setting swaps them for a
    // motor wired the other way round.
    const bool reverse = (m.position == TurnoutPosition::Thrown) != m.reverse;
    channels_.set(channel, ChannelMode::DC, reverse, m.duty);
    channels_.flush();

    if (m.type == MotorType::Pulse) {
        m.pulsing = true;
        m.pulseStart = millis();
    }
}

void Turnouts::release(uint8_t channel) {
    if (channel < NUM_CHANNELS) {
        motor_[channel].pulsing = false;
    }
}

TurnoutPosition Turnouts::position(uint8_t channel) const {
    return channel < NUM_CHANNELS ? motor_[channel].position : TurnoutPosition::Unknown;
}

bool Turnouts::pulsing(uint8_t channel) const {
    return channel < NUM_CHANNELS && motor_[channel].pulsing;
}

void Turnouts::update() {
    const uint32_t now = millis();
    for (uint8_t i = 0; i < NUM_CHANNELS; ++i) {
        Motor& m = motor_[i];
        if (m.pulsing && now - m.pulseStart >= m.pulseMs) {
            m.pulsing = false;
            channels_.set(i, ChannelMode::Off, false, 0);
            channels_.flush();
        }
    }
}
