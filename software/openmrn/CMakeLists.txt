# FemtoLCC — OpenMRN firmware.
#
# An ESP-IDF project (not an Arduino sketch) that runs the full OpenMRN stack
# on the FemtoLCC board. The AOLCB-based firmware lives in ../FemtoLCC and is
# built with arduino-cli; the two are alternatives, not layers.
#
#   Uncommon Models — https://uncommonmodels.com

cmake_minimum_required(VERSION 3.16)

# Where the OpenMRN checkout lives. The Makefile passes this; override with
# `make OPENMRN_PATH=/path/to/openmrn`.
if(NOT DEFINED OPENMRN_PATH)
    if(DEFINED ENV{OPENMRN_PATH})
        set(OPENMRN_PATH $ENV{OPENMRN_PATH})
    else()
        set(OPENMRN_PATH "$ENV{HOME}/git/openmrn")
    endif()
endif()
set(OPENMRN_PATH "${OPENMRN_PATH}" CACHE PATH "Path to the OpenMRN checkout")

if(NOT EXISTS "${OPENMRN_PATH}/src/openlcb/SimpleStack.hxx")
    message(FATAL_ERROR
        "OPENMRN_PATH=${OPENMRN_PATH} does not look like an OpenMRN checkout. "
        "Pass -DOPENMRN_PATH=/path/to/openmrn or set OPENMRN_PATH in the "
        "environment.")
endif()

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(femtolcc-openmrn)
