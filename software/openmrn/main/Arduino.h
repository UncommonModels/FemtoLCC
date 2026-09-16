// The slice of the Arduino API the copied hardware drivers use.
//
// Channels, Occupancy, Turnouts, IoPins and Expander are copies of the files in
// ../FemtoLCC, which is an Arduino sketch. This firmware is a plain ESP-IDF
// project, so there is no Arduino core to call. Rather than editing the copies
// — which would make it impossible to tell a real change in the originals from
// a port artefact — this header provides exactly the functions they call, over
// the ESP-IDF drivers.
//
// It is not an Arduino compatibility layer in any general sense. Everything
// here exists because one of those five files uses it; see ArduinoCompat.cxx.
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
