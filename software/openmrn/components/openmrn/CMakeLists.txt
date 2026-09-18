# OpenMRN as an ESP-IDF component.
#
# This is based on the component definition OpenMRN itself ships in
# etc/esp-idf/CMakeLists.txt, with three deliberate differences:
#
#   1. The source tree is not copied into the project. OPENMRN_PATH points at a
#      checkout — by default the pinned ../openmrn submodule — so the two stay
#      independent and the checkout is never written to.
#   2. A few source groups are dropped — see the notes beside the exclusions.
#   3. Individual files can be replaced by patched copies under
#      ../../patches/overrides, which is how the ESP32-C6 fixes are applied
#      without editing the OpenMRN checkout. patches/README.md explains each one.
#
#   Uncommon Models — https://uncommonmodels.com

set(OPENMRNPATH ${OPENMRN_PATH})
set(OVERRIDE_DIR ${CMAKE_CURRENT_LIST_DIR}/../../patches/overrides)

set(SRCS
    ${OPENMRNPATH}/src/dcc/dcc_constants.cxx
    ${OPENMRNPATH}/src/dcc/DccDebug.cxx
    ${OPENMRNPATH}/src/dcc/Defs.cxx
    ${OPENMRNPATH}/src/dcc/LocalTrackIf.cxx
    ${OPENMRNPATH}/src/dcc/Loco.cxx
    ${OPENMRNPATH}/src/dcc/Packet.cxx
    ${OPENMRNPATH}/src/dcc/RailcomBroadcastDecoder.cxx
    ${OPENMRNPATH}/src/dcc/RailCom.cxx
    ${OPENMRNPATH}/src/dcc/RailcomDebug.cxx
    ${OPENMRNPATH}/src/dcc/SimpleUpdateLoop.cxx
    ${OPENMRNPATH}/src/dcc/UpdateLoop.cxx

    ${OPENMRNPATH}/src/executor/AsyncNotifiableBlock.cxx
    ${OPENMRNPATH}/src/executor/Executor.cxx
    ${OPENMRNPATH}/src/executor/Notifiable.cxx
    ${OPENMRNPATH}/src/executor/Service.cxx
    ${OPENMRNPATH}/src/executor/StateFlow.cxx
    ${OPENMRNPATH}/src/executor/Timer.cxx

    # Esp32HardwareI2C.cxx and Esp32Ledc.cxx are deliberately left out: this
    # firmware drives I2C and the LEDC PWM through the IDF APIs directly (see
    # main/ArduinoCompat.cxx), and both files are extra ESP32-C6 risk for no
    # gain.
    ${OPENMRNPATH}/src/freertos_drivers/esp32/Esp32HardwareTwai.cxx
    ${OPENMRNPATH}/src/freertos_drivers/esp32/Esp32SocInfo.cxx
    ${OPENMRNPATH}/src/freertos_drivers/esp32/Esp32WiFiManager.cxx

    ${OPENMRNPATH}/src/openlcb/AliasAllocator.cxx
    ${OPENMRNPATH}/src/openlcb/AliasCache.cxx
    ${OPENMRNPATH}/src/openlcb/BroadcastTime.cxx
    ${OPENMRNPATH}/src/openlcb/BroadcastTimeClient.cxx
    ${OPENMRNPATH}/src/openlcb/BroadcastTimeDefs.cxx
    ${OPENMRNPATH}/src/openlcb/BroadcastTimeServer.cxx
    ${OPENMRNPATH}/src/openlcb/BulkAliasAllocator.cxx
    ${OPENMRNPATH}/src/openlcb/CanDefs.cxx
    ${OPENMRNPATH}/src/openlcb/ConfigEntry.cxx
    ${OPENMRNPATH}/src/openlcb/ConfigUpdateFlow.cxx
    ${OPENMRNPATH}/src/openlcb/Datagram.cxx
    ${OPENMRNPATH}/src/openlcb/DatagramCan.cxx
    ${OPENMRNPATH}/src/openlcb/DatagramTcp.cxx
    ${OPENMRNPATH}/src/openlcb/DccAccyProducer.cxx
    ${OPENMRNPATH}/src/openlcb/DefaultNode.cxx
    ${OPENMRNPATH}/src/openlcb/DefaultCdi.cxx
    ${OPENMRNPATH}/src/openlcb/EventHandler.cxx
    ${OPENMRNPATH}/src/openlcb/EventHandlerContainer.cxx
    ${OPENMRNPATH}/src/openlcb/EventHandlerTemplates.cxx
    ${OPENMRNPATH}/src/openlcb/EventService.cxx
    # New since the previous baseline: SimpleStack's CanPhysicalIf is built on
    # it, so leaving it out is an undefined reference at link time rather than a
    # missing feature.
    ${OPENMRNPATH}/src/openlcb/FilteringCanHubFlow.cxx
    ${OPENMRNPATH}/src/openlcb/If.cxx
    ${OPENMRNPATH}/src/openlcb/IfCan.cxx
    ${OPENMRNPATH}/src/openlcb/IfImpl.cxx
    ${OPENMRNPATH}/src/openlcb/IfTcp.cxx
    ${OPENMRNPATH}/src/openlcb/MemoryConfig.cxx
    ${OPENMRNPATH}/src/openlcb/nmranet_constants.cxx
    ${OPENMRNPATH}/src/openlcb/Node.cxx
    ${OPENMRNPATH}/src/openlcb/NodeBrowser.cxx
    ${OPENMRNPATH}/src/openlcb/NodeInitializeFlow.cxx
    ${OPENMRNPATH}/src/openlcb/NonAuthoritativeEventProducer.cxx
    ${OPENMRNPATH}/src/openlcb/PIPClient.cxx
    ${OPENMRNPATH}/src/openlcb/RoutingLogic.cxx
    ${OPENMRNPATH}/src/openlcb/SimpleNodeInfo.cxx
    ${OPENMRNPATH}/src/openlcb/SimpleNodeInfoMockUserFile.cxx
    ${OPENMRNPATH}/src/openlcb/SimpleNodeInfoResponse.cxx
    ${OPENMRNPATH}/src/openlcb/SimpleStack.cxx
    ${OPENMRNPATH}/src/openlcb/StreamReceiver.cxx
    ${OPENMRNPATH}/src/openlcb/StreamTransport.cxx
    ${OPENMRNPATH}/src/openlcb/TcpDefs.cxx
    ${OPENMRNPATH}/src/openlcb/TractionCvSpace.cxx
    ${OPENMRNPATH}/src/openlcb/TractionDefs.cxx
    ${OPENMRNPATH}/src/openlcb/TractionProxy.cxx
    ${OPENMRNPATH}/src/openlcb/TractionTestTrain.cxx
    ${OPENMRNPATH}/src/openlcb/TractionThrottle.cxx
    ${OPENMRNPATH}/src/openlcb/TractionTrain.cxx
    ${OPENMRNPATH}/src/openlcb/Velocity.cxx
    ${OPENMRNPATH}/src/openlcb/WriteHelper.cxx

    ${OPENMRNPATH}/src/os/FakeClock.cxx
    ${OPENMRNPATH}/src/os/logging_malloc.cxx
    ${OPENMRNPATH}/src/os/MDNS.cxx
    ${OPENMRNPATH}/src/os/os.c
    ${OPENMRNPATH}/src/os/OSImpl.cxx
    ${OPENMRNPATH}/src/os/OSSelectWakeup.cxx
    ${OPENMRNPATH}/src/os/stack_malloc.c
    ${OPENMRNPATH}/src/os/TempFile.cxx
    ${OPENMRNPATH}/src/os/watchdog.c

    ${OPENMRNPATH}/src/utils/Base64.cxx
    ${OPENMRNPATH}/src/utils/Blinker.cxx
    ${OPENMRNPATH}/src/utils/Buffer.cxx
    ${OPENMRNPATH}/src/utils/CanIf.cxx
    ${OPENMRNPATH}/src/utils/ClientConnection.cxx
    ${OPENMRNPATH}/src/utils/ConfigUpdateListener.cxx
    ${OPENMRNPATH}/src/utils/constants.cxx
    ${OPENMRNPATH}/src/utils/Crc.cxx
    ${OPENMRNPATH}/src/utils/DirectHub.cxx
    ${OPENMRNPATH}/src/utils/DirectHubGc.cxx
    ${OPENMRNPATH}/src/utils/DirectHubLegacy.cxx
    ${OPENMRNPATH}/src/utils/errno_exit.c
    ${OPENMRNPATH}/src/utils/FdUtils.cxx
    ${OPENMRNPATH}/src/utils/FileUtils.cxx
    ${OPENMRNPATH}/src/utils/format_utils.cxx
    ${OPENMRNPATH}/src/utils/ForwardAllocator.cxx
    ${OPENMRNPATH}/src/utils/GcStreamParser.cxx
    ${OPENMRNPATH}/src/utils/GcTcpHub.cxx
    ${OPENMRNPATH}/src/utils/gc_format.cxx
    ${OPENMRNPATH}/src/utils/GridConnect.cxx
    ${OPENMRNPATH}/src/utils/GridConnectHub.cxx
    ${OPENMRNPATH}/src/utils/HubDevice.cxx
    ${OPENMRNPATH}/src/utils/HubDeviceSelect.cxx
    ${OPENMRNPATH}/src/utils/ieeehalfprecision.c
    ${OPENMRNPATH}/src/utils/logging.cxx
    ${OPENMRNPATH}/src/utils/Queue.cxx
    ${OPENMRNPATH}/src/utils/ServiceLocator.cxx
    ${OPENMRNPATH}/src/utils/SocketClient.cxx
    ${OPENMRNPATH}/src/utils/socket_listener.cxx
    ${OPENMRNPATH}/src/utils/Stats.cxx
    ${OPENMRNPATH}/src/utils/StringPrintf.cxx
)

# Sources OpenMRN's own component list carries but this firmware does not need.
# Each is either another platform's transport or a feature this board has no use
# for, and every one of them is a compile risk on a RISC-V target:
#
#   src/ble/*, openlcb/BLEAdvertisement.cxx, openlcb/BLEService.cxx
#                            Bluetooth LE. Dropping them also drops the `bt`
#                            dependency.
#   utils/SocketCan.cxx      Linux SocketCAN.
#   utils/JSHubPort.cxx      emscripten.
#   utils/ReflashBootloader.cxx, utils/DirectHubGc.cxx's bootloader paths
#                            firmware upgrade over CAN, not wired up here.

# ---------------------------------------------------------------------------
# Patched sources
#
# A file listed here is compiled from patches/overrides/<path> instead of from
# the OpenMRN checkout. The override is a copy of the original with a small,
# documented change; patches/*.patch holds the diff and patches/README.md the
# reason. Nothing under OPENMRN_PATH is ever modified.
# ---------------------------------------------------------------------------
set(PATCHED_SOURCES
    freertos_drivers/esp32/Esp32HardwareTwai.cxx
    freertos_drivers/esp32/Esp32SocInfo.cxx
    freertos_drivers/esp32/Esp32WiFiManager.cxx
)

foreach(rel ${PATCHED_SOURCES})
    if(EXISTS "${OVERRIDE_DIR}/${rel}")
        list(REMOVE_ITEM SRCS "${OPENMRNPATH}/src/${rel}")
        list(APPEND SRCS "${OVERRIDE_DIR}/${rel}")
        message(STATUS "OpenMRN: using patched ${rel}")
    endif()
endforeach()

# The override directory comes first so that patched headers shadow the
# originals without the checkout being touched.
set(INCLUDE_DIRS
    ${OVERRIDE_DIR}
    ${OPENMRNPATH}/src
    ${OPENMRNPATH}/include/esp-idf
    ${OPENMRNPATH}/include
)

set(IDF_DEPS
    app_update
    bootloader_support
    driver
    esp_adc
    esp_app_format
    esp_event
    esp_netif
    esp_system
    esp_timer
    esp_wifi
    espcoredump
    hal
    heap
    nvs_flash
    pthread
    spiffs
    vfs
    espressif__mdns)

idf_component_register(SRCS "${SRCS}"
                       INCLUDE_DIRS "${INCLUDE_DIRS}"
                       REQUIRES "${IDF_DEPS}")

###############################################################################
# Warnings.
#
# OpenMRN's own component turns -Werror on; that is not useful here, because the
# interesting failures on a new target are drowned out by deprecation warnings
# from a newer ESP-IDF. These are the suppressions OpenMRN ships for the Arduino
# component.
###############################################################################
target_compile_options(${COMPONENT_LIB} PUBLIC $<$<COMPILE_LANGUAGE:CXX>:-Wno-volatile>)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-type-limits)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-ignored-qualifiers)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)
target_compile_options(${COMPONENT_LIB} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Wno-class-memaccess>)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-implicit-fallthrough)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error=nonnull)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-deprecated-declarations)
target_compile_options(${COMPONENT_LIB} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Wno-redundant-move>)
