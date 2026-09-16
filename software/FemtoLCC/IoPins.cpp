#include "IoPins.h"
#include <Arduino.h>

// One I2C read of GPIOA every this many milliseconds, whatever the debounce
// settings: well under any useful debounce, and cheap on the bus.
static const uint32_t POLL_MS = 5;

static inline uint16_t lineBit(uint8_t line) {
    return (uint16_t)(1u << line);
}

static inline uint16_t withBit(uint16_t mask, uint16_t bit, bool on) {
    return on ? (uint16_t)(mask | bit) : (uint16_t)(mask & ~bit);
}

// --- PinBank -------------------------------------------------------------------

PinBank::PinBank()
    : count_(0), inputs_(0), outputs_(0), invert_(0), on_(0), active_(0), pending_(0),
      transition_(0) {
    for (uint8_t i = 0; i < MAX_LINES; ++i) {
        mode_[i] = PinMode::Unused;
        debounceMs_[i] = 0;
        since_[i] = 0;
    }
}

void PinBank::load(const PinSettings* pins, uint8_t count) {
    count_ = count < MAX_LINES ? count : MAX_LINES;
    uint16_t inputs = 0;
    uint16_t outputs = 0;
    uint16_t invert = 0;
    for (uint8_t i = 0; i < MAX_LINES; ++i) {
        const bool used = i < count_;
        mode_[i] = used ? pins[i].mode : PinMode::Unused;
        debounceMs_[i] = used ? pins[i].debounceMs : 0;
        invert = withBit(invert, lineBit(i), used && pins[i].invert);
        if (mode_[i] == PinMode::Output) {
            outputs |= lineBit(i);
        } else if (mode_[i] != PinMode::Unused) {
            inputs |= lineBit(i);
        }
    }
    on_ &= outputs & outputs_;
    inputs_ = inputs;
    outputs_ = outputs;
    invert_ = invert;
    transition_ = 0;
}

uint16_t PinBank::inputMask() const {
    return (uint16_t)~outputs_;
}

uint16_t PinBank::pullupMask() const {
    uint16_t mask = 0;
    for (uint8_t i = 0; i < MAX_LINES; ++i) {
        if (mode_[i] == PinMode::Unused || mode_[i] == PinMode::InputPullup) {
            mask |= lineBit(i);
        }
    }
    return mask;
}

uint16_t PinBank::latch() const {
    // On pulls the line low, unless inverted. A high latch bit leaves an
    // open-drain output floating.
    return (uint16_t)~((on_ ^ invert_) & outputs_);
}

void PinBank::start(uint16_t levels, bool valid) {
    // Active is low, unless inverted.
    active_ = valid ? (uint16_t)((~levels ^ invert_) & inputs_) : 0;
    pending_ = active_;
    transition_ = 0;
    const uint32_t now = millis();
    for (uint8_t i = 0; i < MAX_LINES; ++i) {
        since_[i] = now;
    }
}

void PinBank::sample(uint16_t levels, uint32_t now) {
    const uint16_t level = (uint16_t)((~levels ^ invert_) & inputs_);
    for (uint8_t i = 0; i < count_; ++i) {
        const uint16_t bit = lineBit(i);
        if (!(inputs_ & bit)) {
            continue;
        }
        const bool high = (level & bit) != 0;
        if (high == ((active_ & bit) != 0)) {
            pending_ = withBit(pending_, bit, high);
            since_[i] = now;
            continue;
        }
        // A new level must hold for the debounce time before it counts.
        if (high != ((pending_ & bit) != 0)) {
            pending_ = withBit(pending_, bit, high);
            since_[i] = now;
        }
        if (now - since_[i] >= debounceMs_[i]) {
            active_ = withBit(active_, bit, high);
            transition_ |= bit;
        }
    }
}

bool PinBank::takeTransition(uint8_t line, bool& active) {
    if (line >= count_ || !(transition_ & lineBit(line))) {
        return false;
    }
    transition_ &= (uint16_t)~lineBit(line);
    active = (active_ & lineBit(line)) != 0;
    return true;
}

PinMode PinBank::mode(uint8_t line) const {
    return line < count_ ? mode_[line] : PinMode::Unused;
}

bool PinBank::isInput(uint8_t line) const {
    return line < count_ && (inputs_ & lineBit(line)) != 0;
}

bool PinBank::active(uint8_t line) const {
    return line < count_ && (active_ & lineBit(line)) != 0;
}

bool PinBank::output(uint8_t line) const {
    return line < count_ && (on_ & lineBit(line)) != 0;
}

bool PinBank::set(uint8_t line, bool on) {
    if (line >= count_ || !(outputs_ & lineBit(line))) {
        return false;
    }
    const uint16_t before = latch();
    on_ = withBit(on_, lineBit(line), on);
    return latch() != before;
}

// --- IoPins: expander port A ---------------------------------------------------

IoPins::IoPins(Expander& expander) : expander_(expander), lastPoll_(0) {}

void IoPins::configure(const PinSettings pins[EXP_GPIO_COUNT]) {
    load(pins, EXP_GPIO_COUNT);

    // Latch before direction, so a pin that becomes an output starts in the
    // state it should be in rather than whatever the latch held.
    expander_.writePortA((uint8_t)latch());
    expander_.setPortAPullups((uint8_t)pullupMask());
    expander_.setPortADirection((uint8_t)inputMask());

    uint8_t raw = 0xFF;
    const bool read = expander_.readPortA(raw);
    start(raw, read);
}

void IoPins::poll() {
    const uint32_t now = millis();
    if (!anyInput() || now - lastPoll_ < POLL_MS) {
        return;
    }
    lastPoll_ = now;

    uint8_t raw;
    if (expander_.readPortA(raw)) {
        sample(raw, now);
    }
}

void IoPins::setOutput(uint8_t pin, bool on) {
    if (set(pin, on)) {
        expander_.writePortA((uint8_t)latch());
    }
}
