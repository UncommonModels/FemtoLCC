---
title: "Hardware"
description: "What each block on the board does, and how a DCC signal becomes four driven outputs."
weight: 1
---

FemtoLCC is a layout accessory node. It listens on the LCC bus, watches an isolated
feed of the DCC track signal, and drives four independent block outputs. Everything
below traces that path, from the bus in to the terminal blocks out.

## Signal path

```
   LCC bus  ──▶  CAN transceiver  ──▶┐
   (RJ45)        MCP2562             │
                                     ├──▶  ESP32-C6  ──▶  4:1 mux  ──▶  H-bridge  ──▶  Block
   DCC track ─▶  optocoupler      ──▶┘     controller      ×4          driver ×4       outputs
   (terminals)   HCPL-0630                                                             J5–J8
```

## Controller

An **ESP32-C6-WROOM-1** module runs the node. It is a RISC-V part with Wi-Fi, Bluetooth
LE and 802.15.4, so the node can join the layout over the wired LCC bus and still be
reachable wirelessly for configuration or firmware updates.

Programming and console access come out on a 6-pin UART header, with a USB-C port
alongside it. A push button pulls the module's `EN` line to ground for a manual reset.

## LCC bus interface

LCC — Layout Command Control, the NMRA's OpenLCB standard — runs over CAN. An
**MCP2562** transceiver drives the bus, and it lands on a **dual-port RJ45 jack**. Two
ports rather than one is the important detail: the node daisy-chains the bus rather than
terminating it, so you run the cable in one side and straight out the other to the next
node. A 120 Ω termination resistor is fitted for use at the end of a run.

## DCC isolation

The DCC signal on the track carries the full track voltage and is electrically nothing
like the node's logic supply, so it is never connected directly. An **HCPL-0630** dual
optocoupler sits between the two, fed through 2 kΩ 1 W wirewound-class resistors sized
for track voltage. The optocoupler passes the signal across as light, leaving no
conductive path between track and logic.

This is what lets the node share a common ground with the LCC bus while the track floats
at its own potential.

## Block driver channels

Four identical channels, one per output. Each pairs an **SN74HC253** dual 4:1
multiplexer with a **DRV8874** H-bridge driver:

- the **multiplexer** selects which signal reaches the driver — the isolated DCC feed, a
  static level, or a control signal from the controller
- the **H-bridge** does the driving, capable of well over what a block section needs, with
  current sensing on each channel so the controller can tell how much is being drawn

Each channel gets its own local decoupling — a 10 µF bulk capacitor plus 100 nF and 22 nF
close to the driver's supply pins — because an H-bridge switching a block draws current in
sharp steps.

Outputs land on **five-millimetre-pitch screw terminals**, `J5` through `J8`, one pair per
block.

## I/O expansion

An **MCP23018** port expander adds sixteen general-purpose I/O lines over I²C, for
whatever the controller's own pins do not cover. A **PMOD socket** and a 4-pin JST header
break out further connections for daughterboards and sensors.

## Power

Input arrives on a 3.5 mm pluggable terminal header. A **P-channel MOSFET** in the input
path acts as an ideal diode, protecting the board against reversed supply leads without
the voltage drop a plain diode would cost.

Two linear regulators follow: **5 V** for the driver logic and **3.3 V** for the
controller and the digital side. The USB-C port can supply the board as an alternative to
the terminal input.

## Connectors at a glance

| Connector | What it is |
|---|---|
| `J2` | Dual-port RJ45 — the LCC bus, in and out |
| `J5`–`J8` | Four block outputs, 0.200 in pitch screw terminals |
| `J9`, `J10` | DCC and power input, 0.138 in pluggable headers |
| `P1` | USB-C — power and programming |
| `J1` | 6-pin UART header |
| `J4` | PMOD socket |
| `J3` | 4-pin JST SH, top entry |

Board outline, mounting pattern and drill details are on the
[mechanical page](../mechanical/).
