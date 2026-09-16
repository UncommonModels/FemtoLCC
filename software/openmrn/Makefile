# FemtoLCC — OpenMRN firmware.
#
# A wrapper round idf.py. Self-contained: it does not use, and is not used by,
# ../Makefile, which builds the AOLCB firmware with arduino-cli.
#
#   make                 what the targets are
#   make build           build for the C6 (the board)
#   make TARGET=esp32 build   build for the fallback target
#
# Only one firmware can be on the board at a time. See README.md.

# The board is an ESP32-C6. TARGET=esp32 is the documented fallback; see the
# "ESP32-C6 support" section of README.md for what that is for.
TARGET      ?= esp32c6

IDF_PATH    ?= $(HOME)/esp/esp-idf
OPENMRN_PATH?= $(HOME)/git/openmrn

# One build directory and one sdkconfig per target, so switching targets does
# not force a full reconfigure of the other.
BUILD_DIR   := build-$(TARGET)

# The machine this was developed on is short of memory and OpenMRN is about 130
# translation units. Raise it if you have the RAM.
JOBS        ?= 2

# The board's native USB Serial/JTAG port enumerates as ttyACM. Flashing and
# the monitor are here for completeness; neither is run as part of a build.
PORT        ?= /dev/ttyACM0

IDF_ENV     := . $(IDF_PATH)/export.sh >/dev/null 2>&1
IDF_ARGS    := -B $(BUILD_DIR) \
               -DIDF_TARGET=$(TARGET) \
               -DSDKCONFIG=$(BUILD_DIR)/sdkconfig \
               -DOPENMRN_PATH=$(OPENMRN_PATH)

.DEFAULT_GOAL := help

## help: list the available targets
help:
	@echo 'FemtoLCC OpenMRN firmware targets:'
	@sed -n 's/^## \([a-z-]*\): /  \1|/p' $(MAKEFILE_LIST) | column -t -s'|'
	@echo
	@echo 'Override TARGET=, IDF_PATH=, OPENMRN_PATH=, JOBS=, PORT= as needed.'
	@echo 'Current: TARGET=$(TARGET) BUILD_DIR=$(BUILD_DIR)'

## build: compile the firmware
# idf.py in ESP-IDF 5.2 has no --jobs flag, so CMake is run through idf.py (which
# knows the target and the component set) and ninja is then driven directly,
# which is the only way to cap the parallelism.
build:
	$(IDF_ENV) && idf.py $(IDF_ARGS) reconfigure && ninja -C $(BUILD_DIR) -j$(JOBS)

## reconfigure: re-run CMake without building
reconfigure:
	$(IDF_ENV) && idf.py $(IDF_ARGS) reconfigure

## size: report flash and RAM use
size:
	$(IDF_ENV) && idf.py $(IDF_ARGS) size

## size-components: flash and RAM use, broken down by component
size-components:
	$(IDF_ENV) && idf.py $(IDF_ARGS) size-components

## menuconfig: edit this target's sdkconfig
menuconfig:
	$(IDF_ENV) && idf.py $(IDF_ARGS) menuconfig

## flash: build, then upload over USB
flash:
	$(IDF_ENV) && idf.py $(IDF_ARGS) -p $(PORT) flash

## monitor: open the serial console
monitor:
	$(IDF_ENV) && idf.py $(IDF_ARGS) -p $(PORT) monitor

## sync-drivers: refresh the hardware driver copies from ../FemtoLCC
sync-drivers:
	./sync-drivers.sh

## check-drivers: report which driver copies differ from ../FemtoLCC
check-drivers:
	./sync-drivers.sh --check

## clean: remove this target's build directory
clean:
	rm -rf $(BUILD_DIR)

## distclean: remove every build directory and fetched component
distclean:
	rm -rf build-* managed_components dependencies.lock sdkconfig sdkconfig.old

.PHONY: help build reconfigure size size-components menuconfig flash monitor \
        sync-drivers check-drivers clean distclean
