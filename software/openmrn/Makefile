# FemtoLCC — OpenMRN firmware.
#
# A wrapper round idf.py. This is the board's firmware; the repository's
# top-level Makefile delegates `make firmware`, `make flash`, `make monitor`
# and `make firmware-deps` here.
#
#   make                 what the targets are
#   make build           build for the C6 (the board)
#   make TARGET=esp32 build   build for the fallback target
#
# See README.md.

# The board is an ESP32-C6. TARGET=esp32 is the documented fallback; see the
# "ESP32-C6 support" section of README.md for what that is for.
TARGET      ?= esp32c6

IDF_PATH    ?= $(HOME)/esp/esp-idf

# Where this Makefile lives, and the top of the repository above it.
HERE        := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
REPO        := $(abspath $(HERE)/../..)

# OpenMRN is a pinned git submodule at ./openmrn, so a clone of this repository
# builds against exactly the revision this firmware was tested with:
#
#   git submodule update --init --recursive
#
# Override OPENMRN_PATH to build against a checkout of your own instead.
OPENMRN_PATH?= $(HERE)/openmrn

# One build directory and one sdkconfig per target, so switching targets does
# not force a full reconfigure of the other.
BUILD_DIR   := build-$(TARGET)

# The machine this was developed on is short of memory and OpenMRN is about 130
# translation units. Raise it if you have the RAM.
JOBS        ?= 2

# The board's native USB Serial/JTAG port enumerates as ttyACM. Flashing and
# the monitor are here for completeness; neither is run as part of a build.
# Flashing uses the USB-C port at P1, which the ROM bootloader answers on. The
# console is NOT there: P1 carries GridConnect, so `make monitor` wants whatever
# USB-serial adapter is on the J1 header instead.
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

## deps: fetch the OpenMRN submodule, and check for an ESP-IDF install
# ESP-IDF itself is not installed from here. It is a large, shared toolchain
# that lives outside the repository, so this fetches what the repository owns
# and tells you what to run for the rest.
deps:
	git -C $(REPO) submodule update --init --recursive
	@if [ -f "$(IDF_PATH)/export.sh" ]; then \
	    echo "ESP-IDF found at $(IDF_PATH)."; \
	    echo "If the $(TARGET) toolchain is missing, run:"; \
	    echo "    (cd $(IDF_PATH) && ./install.sh $(TARGET))"; \
	else \
	    echo "No ESP-IDF at $(IDF_PATH)."; \
	    echo "Install v5.2, run ./install.sh $(TARGET), or set IDF_PATH."; \
	    echo "See README.md."; \
	    exit 1; \
	fi

## clean: remove this target's build directory
clean:
	rm -rf $(BUILD_DIR)

## distclean: remove every build directory and fetched component
distclean:
	rm -rf build-* managed_components dependencies.lock sdkconfig sdkconfig.old

.PHONY: help build reconfigure size size-components menuconfig flash monitor \
        deps clean distclean
