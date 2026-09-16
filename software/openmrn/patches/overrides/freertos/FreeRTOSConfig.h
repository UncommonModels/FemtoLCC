// Forwarding shim: use ESP-IDF's FreeRTOSConfig.h, not OpenMRN's.
//
// OpenMRN's include/ directory has to be on the include path so its sources can
// find openmrn_features.h, can_frame.h and nmranet_config.h. It also contains
// include/freertos/FreeRTOSConfig.h, a configuration for bare-metal FreeRTOS
// ports. Because the component's own include directories are searched before
// those of the components it requires, that file is what ESP-IDF's esp_task.h
// picks up when it asks for <freertos/FreeRTOSConfig.h>, giving
//
//   include/freertos/FreeRTOSConfig.h:301: #error please provide the
//   FreeRTOSConfig.h for your target
//
// plus a pile of configTICK_RATE_HZ / configUSE_IDLE_HOOK redefinition
// warnings against ESP-IDF's real one.
//
// This file is earlier still on the include path and forwards to ESP-IDF's,
// which is reachable unqualified because ESP-IDF also puts
// components/freertos/config/include/freertos on the include path. The angle
// brackets matter: they stop this file from including itself.
//
// NOT an ESP32-C6 problem. This happens on any ESP-IDF target and is really a
// packaging clash between OpenMRN's include/ layout and ESP-IDF 5.x.
#pragma once
#include <FreeRTOSConfig.h>
