// The slice of the Arduino API the hardware drivers use.
//
// Channels, Occupancy, Turnouts, IoPins, Expander and DCCSource are written
// against the Arduino API, because that is the API they were first written
// for. This firmware is a plain ESP-IDF project with no Arduino core, so this
// header declares exactly the calls those files make and ArduinoCompat.cxx
// implements them over the ESP-IDF drivers. Keeping the drivers on this narrow
// API, rather than rewriting them against ESP-IDF, is deliberate: they are
// working, tested code, and the shim is small enough to read in one sitting.
//
// It is not an Arduino compatibility layer in any general sense. Everything
// here exists because one of those files uses it; a driver that needs a call
// not listed here means adding it here and in ArduinoCompat.cxx.
//
//   Uncommon Models — https://uncommonmodels.com

#pragma once

#include <stdint.h>
#include <stddef.h>

// Pin modes, as Arduino names them.
#define INPUT           0x01
#define OUTPUT          0x03
#define INPUT_PULLUP    0x05

#define LOW             0x0
#define HIGH            0x1

/// Milliseconds since boot. Backed by esp_timer, so it does not wrap for
/// several hundred thousand years; the drivers' `now - since` arithmetic is
/// wrap-safe anyway.
uint32_t millis(void);

/// Microseconds since boot.
uint32_t micros(void);

/// Busy-waits. Only used at start-up.
void delay(uint32_t ms);

/// Busy-waits for microseconds. The PCA9685 driver needs short settling waits
/// after a register write, far below a FreeRTOS tick.
void delayMicroseconds(uint32_t us);

/// Sets a GPIO's direction and pull-up.
void pinMode(int pin, uint8_t mode);

/// Drives a GPIO that pinMode() set to OUTPUT.
void digitalWrite(int pin, uint8_t value);

/// Reads a GPIO's level.
int digitalRead(int pin);

/// One 12-bit ADC1 conversion on `pin`, at 12 dB attenuation so the full 3.3 V
/// sense range is usable. Returns 0..4095, which is the scale
/// Channels::currentMilliamps and Occupancy assume.
///
/// Only the four current-sense pins (GPIO 0-3, ADC1 channels 0-3) are set up;
/// any other pin reads 0.
int analogRead(int pin);

/// Attaches an LEDC channel to `pin` at `freq` Hz and `resolution_bits` of
/// duty resolution, as the Arduino-ESP32 3.x API does. Channels are handed out
/// in the order attached.
bool ledcAttach(int pin, uint32_t freq, uint8_t resolution_bits);

/// Sets the duty on a pin previously passed to ledcAttach().
bool ledcWrite(int pin, uint32_t duty);

/// Somewhere text can be printed. IoBoards::printStatus and
/// ServoBoards::printStatus take one of these; this implementation writes to
/// the console. Only printf() is used by the hardware drivers, but print() and
/// println() are here so the class behaves as an Arduino Print does.
class Print
{
public:
    virtual ~Print() = default;
    virtual int printf(const char *format, ...)
        __attribute__((format(printf, 2, 3)));
    virtual size_t print(const char *s);
    virtual size_t println(const char *s = "");
};

/// The console, for printStatus and anything else wanting a Print.
extern Print Console;

// ---------------------------------------------------------------------------
// The hardware timer
//
// DCCSource generates the DCC waveform by flipping channel A's direction line
// from a timer interrupt, re-arming the alarm each time because a '1' half-bit
// is 58 us and a '0' is 100 us. The Arduino-ESP32 3.x timer API it calls maps
// almost one for one onto ESP-IDF's gptimer.
// ---------------------------------------------------------------------------

/// What Arduino hands back from timerBegin(). Opaque to the caller.
struct hw_timer_t;

/// Starts a timer counting up at `frequency` Hz. DCCSource asks for 1 MHz, so
/// one tick is one microsecond. Null if no timer could be claimed.
hw_timer_t *timerBegin(uint32_t frequency);

/// Attaches the interrupt handler. The callback runs in interrupt context.
void timerAttachInterrupt(hw_timer_t *timer, void (*fn)(void));

/// Detaches it again.
void timerDetachInterrupt(hw_timer_t *timer);

/// Sets the alarm `ticks` from the current count. `autoreload` false makes it
/// one-shot, which is what a varying half-bit needs; `reload_count` is the
/// value to reload when it is true.
void timerAlarm(hw_timer_t *timer, uint64_t ticks, bool autoreload,
    uint64_t reload_count);

/// Sets the counter.
void timerWrite(hw_timer_t *timer, uint64_t value);

/// Stops the timer and releases it.
void timerEnd(hw_timer_t *timer);

/// Arduino's marker for code that must live in RAM rather than be paged in
/// from flash. ESP-IDF spells it the same way; esp_attr.h defines it, and this
/// falls back only if that header has not been seen.
#ifndef IRAM_ATTR
#define IRAM_ATTR __attribute__((section(".iram1")))
#endif

/// Arduino's flash-string wrapper. On the ESP32 string literals are already in
/// flash, so this does nothing; it is here so driver code using F("...")
/// compiles.
#define F(string_literal) (string_literal)
