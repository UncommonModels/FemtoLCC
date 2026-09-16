// FemtoLCC board pin map.
//
// Every assignment here was read out of the netlist in FemtoLCC.kicad_pcb, not
// transcribed by hand. Net names from the schematic are given alongside so the
// two can be checked against each other.
//
//   Uncommon Models — https://uncommonmodels.com

#pragma once

#include <stdint.h>

// ---------------------------------------------------------------------------
// LCC / CAN bus  — MCP2562 transceiver out to the dual-port RJ45 at J2
// ---------------------------------------------------------------------------
static const int PIN_CAN_TX = 18;   // /cantx
static const int PIN_CAN_RX = 19;   // /canrx

// LCC runs at 125 kbit/s. This is fixed by the standard; do not change it.
static const uint32_t LCC_BITRATE = 125000;

// ---------------------------------------------------------------------------
// DCC input — HCPL-0630 optocoupler, track side is galvanically isolated
// ---------------------------------------------------------------------------
static const int PIN_DCC_IN = 5;    // /dcc_p, the positive half of the opto pair

// ---------------------------------------------------------------------------
// I2C — MCP23018 port expander at U3
// ---------------------------------------------------------------------------
static const int PIN_I2C_SDA = 20;  // /i2c_sda
static const int PIN_I2C_SCL = 21;  // /i2c_scl
static const int PIN_I2C_INT = 22;  // /I2Cint, INTA from the expander

// The MCP23018 address is strapped by the ADDR pin. 0x20 is the base address.
static const uint8_t MCP23018_ADDR = 0x20;

// ---------------------------------------------------------------------------
// UART — 6-pin programming and console header J1
// ---------------------------------------------------------------------------
static const int PIN_UART_TX = 16;  // /uart_tx
static const int PIN_UART_RX = 17;  // /uart_rx

// ---------------------------------------------------------------------------
// Block driver channels
//
// Each channel is an SN74HC253 dual 4:1 multiplexer feeding a DRV8874 H-bridge.
// The multiplexer decides what reaches the driver:
//
//   dcc_en  dir  |  IN1        IN2        meaning
//   -------------+----------------------------------------------------------
//      0      0  |  pwm        (low)      DC drive, forward
//      0      1  |  (low)      pwm        DC drive, reverse
//      1      0  |  dcc_p      dcc_n      DCC pass-through, normal polarity
//      1      1  |  dcc_n      dcc_p      DCC pass-through, reversed polarity
//
// So dcc_en picks the mode and dir picks direction (DC) or phase (DCC). The
// reversed-polarity case is what lets a block act as a reversing section.
//
// pwm and sense are on the ESP32 for all four channels. dir is on the ESP32 for
// channel A only; B, C and D live on the expander, as do all four dcc_en lines.
// ---------------------------------------------------------------------------
static const int NUM_CHANNELS = 4;

// PWM into the multiplexer's C0/C1 inputs — /pwm_A … /pwm_D
static const int PIN_PWM[NUM_CHANNELS]     = { 8, 10, 11, 15 };

// DRV8874 IPROPI current sense, on ADC1 — /sense_A … /sense_D
static const int PIN_SENSE[NUM_CHANNELS]   = { 0, 1, 2, 3 };

// DRV8874 nFAULT, open drain, active low — /nFault_A … /nFault_D
static const int PIN_NFAULT[NUM_CHANNELS]  = { 6, 7, 9, 23 };

// Channel A's direction line is wired straight to the ESP32; -1 means the
// channel's dir line is on the expander instead.
static const int PIN_DIR_A = 4;             // /dir_A
static const int PIN_DIR[NUM_CHANNELS]     = { PIN_DIR_A, -1, -1, -1 };

// MCP23018 port B bit positions.
static const uint8_t EXP_DCC_EN_BIT[NUM_CHANNELS] = { 0, 1, 2, 3 };  // GPB0..GPB3
static const uint8_t EXP_DIR_BIT[NUM_CHANNELS]    = { 0xFF, 5, 6, 7 };// GPB5..GPB7
static const uint8_t EXP_LED_BIT = 4;                                 // GPB4 -> D4

// Port A (GPA0..GPA7) is uncommitted general-purpose I/O, brought out as /P0../P7.
static const uint8_t EXP_GPIO_COUNT = 8;
