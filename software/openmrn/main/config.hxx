// The CDI root: what a configuration tool sees, and the layout of memory
// space 253.
//
// The settings groups themselves are in FemtoCdi.hxx. This file assembles them
// into the segment and the CDI root.
//
// Note that SNIP_STATIC_DATA is *not* defined here: it is defined once, in
// main.cxx, because this header is included by more than one translation unit.
//
//   Uncommon Models — https://uncommonmodels.com

#ifndef _FEMTOLCC_CONFIG_HXX_
#define _FEMTOLCC_CONFIG_HXX_

#include "openlcb/ConfigRepresentation.hxx"
#include "openlcb/MemoryConfig.hxx"

#include "freertos_drivers/esp32/Esp32WiFiConfiguration.hxx"

#include "FemtoCdi.hxx"
#include "board.h"

namespace openlcb
{

/// Bump this whenever the layout below changes, so that a board updated from an
/// older firmware resets its settings instead of misreading them.
static constexpr uint16_t CANONICAL_VERSION = 0x0102;

/// The four outputs, A-D at J5-J8. The form numbers them 1 to 4.
using AllChannels = RepeatedGroup<ChannelConfig, NUM_CHANNELS>;

/// The eight I/O pins, P0-P7 on the expander's port A. The form numbers them
/// 1 to 8, so pin 1 is P0.
using AllPins = RepeatedGroup<PinConfig, EXP_GPIO_COUNT>;

/// Up to four I/O expansion boards and two servo/light boards on J3.
using AllIoBoards = RepeatedGroup<IoBoardConfig, NUM_XIO_BOARDS>;
using AllServoBoards = RepeatedGroup<ServoBoardConfig, NUM_SERVO_BOARDS>;

/// The settings segment. It starts at offset 128 to leave room for the ACDI
/// user data below it.
CDI_GROUP(FemtoLccSegment, Segment(MemoryConfigDefs::SPACE_CONFIG), Offset(128));
CDI_GROUP_ENTRY(internal_config, InternalConfigData);
CDI_GROUP_ENTRY(channels, AllChannels, Name("Outputs"), RepName("Output"));
CDI_GROUP_ENTRY(pins, AllPins, Name("I/O pins"), RepName("Pin"));
CDI_GROUP_ENTRY(io_boards, AllIoBoards, Name("I/O expansion boards"),
    RepName("Board"));
CDI_GROUP_ENTRY(servo_boards, AllServoBoards, Name("Servo and light boards"),
    RepName("Board"));
CDI_GROUP_ENTRY(wifi_credentials, WiFiCredentialsConfig, Name("WiFi network"));
/// OpenMRN's own WiFi group: whether to be a hub, whether to connect out to
/// one, the ports and the power-save setting.
CDI_GROUP_ENTRY(wifi, WiFiConfiguration, Name("WiFi hub and uplink"));
CDI_GROUP_END();

/// The root of the CDI.
CDI_GROUP(ConfigDef, MainCdi());
CDI_GROUP_ENTRY(ident, Identification);
CDI_GROUP_ENTRY(acdi, Acdi);
CDI_GROUP_ENTRY(userinfo, UserInfoSegment, Name("Node"));
CDI_GROUP_ENTRY(seg, FemtoLccSegment, Name("Settings"));
CDI_GROUP_END();

} // namespace openlcb

#endif // _FEMTOLCC_CONFIG_HXX_
