# FemtoLCC firmware.
#
# Standalone, or driven from the top-level Makefile as `make firmware`,
# `make flash`, `make monitor`.

SKETCH      := FemtoLCC
FQBN        ?= esp32:esp32:esp32c6
PORT        ?= /dev/ttyUSB0
BAUD        ?= 115200
ARDUINO_CLI ?= arduino-cli

# AOLCB supplies the OpenLCB stack. Point AOLCB_DIR at a checkout, or leave it
# unset once the library is installed through the library manager.
AOLCB_DIR   ?= $(HOME)/git/AOLCB
LIBFLAG     := $(if $(wildcard $(AOLCB_DIR)),--library $(AOLCB_DIR),)

BUILD_DIR   := build
CORE        := esp32:esp32

SOURCES := $(wildcard $(SKETCH)/*.ino $(SKETCH)/*.cpp $(SKETCH)/*.h)

.DEFAULT_GOAL := help

## help: list the available targets
help:
	@echo 'FemtoLCC firmware targets:'
	@sed -n 's/^## \([a-z-]*\): /  \1|/p' $(MAKEFILE_LIST) | column -t -s'|'
	@echo
	@echo 'Override FQBN=, PORT=, AOLCB_DIR= as needed.'

## build: compile the firmware
build: $(BUILD_DIR)/.stamp
$(BUILD_DIR)/.stamp: $(SOURCES)
	$(ARDUINO_CLI) compile --fqbn $(FQBN) $(LIBFLAG) \
		--build-path $(BUILD_DIR) $(SKETCH)
	@touch $@

## flash: build, then upload over USB or the UART header
flash: build
	$(ARDUINO_CLI) upload --fqbn $(FQBN) -p $(PORT) \
		--input-dir $(BUILD_DIR) $(SKETCH)

## monitor: open the serial console
monitor:
	$(ARDUINO_CLI) monitor -p $(PORT) --config baudrate=$(BAUD)

## size: report flash and RAM use
size: build
	@$(ARDUINO_CLI) compile --fqbn $(FQBN) $(LIBFLAG) \
		--build-path $(BUILD_DIR) $(SKETCH) 2>&1 \
		| grep -E 'Sketch uses|Global variables'

## deps: install the board core the firmware needs
# Large: roughly 900 MB of downloads and several GB unpacked, because the core
# carries both the Xtensa and RISC-V toolchains.
deps:
	$(ARDUINO_CLI) core update-index
	$(ARDUINO_CLI) core install $(CORE)

## lint: compile with warnings turned up
lint:
	$(ARDUINO_CLI) compile --fqbn $(FQBN) $(LIBFLAG) --warnings all $(SKETCH)

## clean: remove build artefacts
clean:
	rm -rf $(BUILD_DIR)

.PHONY: help build flash monitor size deps lint clean
