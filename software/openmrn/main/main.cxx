// FemtoLCC — OpenMRN firmware.
//
// Brings up an OpenLCB node on the FemtoLCC board using the full OpenMRN stack
// as an ESP-IDF component.
//
//   Uncommon Models — https://uncommonmodels.com

#include <memory>
#include <stdio.h>
#include <string.h>
#include <string>

#include "driver/usb_serial_jtag.h"
#include "esp_mac.h"
#include "esp_spiffs.h"
#include "esp_vfs_dev.h"
#include "esp_vfs_usb_serial_jtag.h"
#include "nvs_flash.h"

#include "freertos_drivers/esp32/Esp32HardwareTwai.hxx"
#include "freertos_drivers/esp32/Esp32SocInfo.hxx"
#include "freertos_drivers/esp32/Esp32WiFiManager.hxx"
#include "openlcb/SimpleStack.hxx"
#include "CDIXMLGenerator.hxx"

#include "FemtoController.hxx"
#include "board.h"
#include "config.hxx"
#include "utils/constants.hxx"

// OpenMRN sizes its memory-space registry from a constant and allocates the
// entries from a fixed pool, so running out asserts in Allocator.hxx during
// start-up rather than failing an insert. The default of five is exactly what
// the stack itself takes - CDI, all-memory, config and the two ACDI spaces -
// leaving nothing for live control (0xE0), module storage (0xE1) and the
// write-only password wrapper.
OVERRIDE_CONST(num_memory_spaces, 12);

// ---------------------------------------------------------------------------
// Node identity
// ---------------------------------------------------------------------------

/// Uncommon Models' part of the node ID — 02.01.57 — in the top three bytes.
/// The bottom three come from the chip.
static constexpr uint64_t NODE_ID_PREFIX = UINT64_C(0x020157) << 24;

/// The node ID every FemtoLCC used to share. It is used only if the MAC cannot
/// be read, which should not happen on a programmed part.
static constexpr uint64_t NODE_ID_FALLBACK = NODE_ID_PREFIX | UINT64_C(1);

/// Derives this board's node ID from its chip.
///
/// The ID is 02.01.57 followed by the low three bytes of the base MAC address,
/// so two boards on one bus are different nodes without anyone assigning IDs by
/// hand: a board whose base MAC is 98:a3:16:a7:cd:94 is node 02.01.57.A7.CD.94.
///
/// This runs during static construction, because NODE_ID below is initialised
/// from it and `stack` is constructed from NODE_ID. That is safe rather than
/// hopeful: ESP-IDF runs the global constructors from the main task after the
/// core system is up, and esp_read_mac() only reads eFuse — no
/// heap, no NVS, no driver. Deriving it any later is not an option, because
/// `stack` is a file-scope object whose address FemtoController takes at file
/// scope too; the node ID has to be known by the time that object is built.
static uint64_t femto_node_id()
{
    // ESP_MAC_BASE, not esp_efuse_mac_get_default(): on a part with 802.15.4 —
    // the C6 is one — that splices the two-byte MAC_EXT into the middle and
    // returns the eight-byte EUI-64, which overruns this buffer and puts the
    // constant ff:fe where the unique bytes belong, so every board comes out as
    // 02.01.57.FF.FE.xx. Measured on hardware before it was caught.
    uint8_t mac[6] = {0, 0, 0, 0, 0, 0};
    const esp_err_t err = esp_read_mac(mac, ESP_MAC_BASE);
    if (err != ESP_OK)
    {
        printf("could not read the base MAC (%s); falling back to node ID "
               "02.01.57.00.00.01\n",
            esp_err_to_name(err));
        return NODE_ID_FALLBACK;
    }
    return NODE_ID_PREFIX | ((uint64_t)mac[3] << 16) |
        ((uint64_t)mac[4] << 8) | (uint64_t)mac[5];
}

/// This board's node ID. Defined before `stack` below, which is what fixes the
/// order the two are initialised in.
static const uint64_t NODE_ID = femto_node_id();

namespace openlcb
{
/// Node identification, returned by Simple Node Information and written into
/// the generated CDI.
///
/// Manufacturer, model and hardware version identify the board as a product to
/// a configuration tool; the software version string identifies the build of
/// this firmware that is on it.
extern const SimpleNodeStaticValues SNIP_STATIC_DATA =
{
    4,
    "Uncommon Models",
    "FemtoLCC",
    "v1",
    "OpenMRN " FEMTOLCC_OPENMRN_VERSION
};
} // namespace openlcb

openlcb::SimpleCanStack stack(NODE_ID);

/// ConfigDef comes from config.hxx. The zero offset is ignored.
static constexpr openlcb::ConfigDef cfg(0);

/// Where SPIFFS is mounted.
static constexpr const char *FS_MOUNT = "/fs";

/// The generated CDI, served from memory space 0xFF.
static constexpr const char *CDI_FILENAME = "/fs/cdi.xml";

namespace openlcb
{
/// The CDI is generated into SPIFFS at boot rather than compiled in, so the
/// statically linked copy is empty.
extern const char CDI_DATA[] = "";

/// Every setting lives in this file; memory space 253 is a window onto it.
extern const char *const CONFIG_FILENAME = "/fs/openlcb_config";

/// How much of it the node exports.
extern const size_t CONFIG_FILE_SIZE = cfg.seg().size() + cfg.seg().offset();

/// The node name and description live in the same file.
extern const char *const SNIP_DYNAMIC_FILENAME = CONFIG_FILENAME;
} // namespace openlcb

/// Hides the WiFi password from anyone reading the configuration space.
///
/// The stack serves the settings file as memory space 0xFD. Everything in that
/// file is readable, which for a WiFi password is the wrong default: a
/// configuration tool only ever needs to *write* it. This wraps the space the
/// stack registered and blanks the password's bytes on the way out; writes pass
/// straight through, and every other byte is untouched.
///
/// Registering it works because MemoryConfigHandler's registry assigns into a
/// map, so inserting a second space for 0xFD after the stack has registered its
/// own replaces it rather than colliding.
///
/// The firmware itself never reads the password through this space — start_wifi()
/// reads the settings file directly — so hiding it costs nothing.
class WriteOnlyPasswordSpace : public openlcb::MemorySpace
{
public:
    /// @param wrapped is the space the stack registered for 0xFD.
    /// @param at is the offset of the password within the file.
    /// @param len is its length in bytes.
    WriteOnlyPasswordSpace(
        openlcb::MemorySpace *wrapped, address_t at, unsigned len)
        : wrapped_(wrapped)
        , at_(at)
        , len_(len)
    {
    }

    address_t max_address() override
    {
        return wrapped_->max_address();
    }

    address_t min_address() override
    {
        return wrapped_->min_address();
    }

    bool read_only() override
    {
        return false;
    }

    size_t write(address_t destination, const uint8_t *data, size_t len,
        errorcode_t *error, Notifiable *again) override
    {
        return wrapped_->write(destination, data, len, error, again);
    }

    size_t read(address_t source, uint8_t *dst, size_t len, errorcode_t *error,
        Notifiable *again) override
    {
        const size_t got = wrapped_->read(source, dst, len, error, again);

        // Blank whatever part of this read overlaps the password. A tool then
        // sees an empty string rather than a short read, which is what keeps
        // the rest of the group readable.
        const address_t from = source;
        const address_t to = source + got;
        if (got && from < at_ + len_ && to > at_)
        {
            const address_t lo = from > at_ ? from : at_;
            const address_t hi = to < at_ + len_ ? to : at_ + len_;
            memset(dst + (lo - from), 0, hi - lo);
        }
        return got;
    }

private:
    openlcb::MemorySpace *wrapped_;
    address_t at_;
    unsigned len_;
};

/// The ESP32 TWAI controller, wired to the MCP2562 at J2. Note the argument
/// order: receive pin first.
Esp32HardwareTwai twai(PIN_CAN_RX, PIN_CAN_TX);

/// The board: its settings, its hardware and its events.
FemtoController controller(&stack, cfg);

/// WiFi, if it is switched on. Built at run time because Esp32WiFiManager takes
/// the network name and password as constructor arguments, and those are
/// settings.
static std::unique_ptr<Esp32WiFiManager> wifiManager;
static std::string wifiSsid;
static std::string wifiPassword;
static std::string wifiHostname;

// ---------------------------------------------------------------------------

/// Mounts the SPIFFS partition that holds the CDI and the configuration.
static void mount_filesystem()
{
    esp_vfs_spiffs_conf_t conf;
    memset(&conf, 0, sizeof(conf));
    conf.base_path = FS_MOUNT;
    conf.partition_label = "spiffs";
    conf.max_files = 8;
    conf.format_if_mount_failed = true;

    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK)
    {
        printf("SPIFFS mount failed (%s); the node cannot store settings.\n",
            esp_err_to_name(err));
        abort();
    }
}

/// Reads the WiFi network out of the configuration and, if it is switched on,
/// starts the WiFi manager.
///
/// This happens before the stack starts because Esp32WiFiManager wants the
/// credentials up front. Changing them therefore needs a restart.
///
/// The connection mode passed to the manager is only the default for the CDI's
/// "connection mode" field: whatever the settings file says wins at the first
/// configuration load, so this decides only what happens before that.
/// CONN_MODE_UPLINK_ONLY is that field's own default (1, "Uplink only") and the
/// value FemtoController's factory reset writes, so an unconfigured board
/// connects out to a hub rather than being one.
static void start_wifi(int fd)
{
    const auto creds = cfg.seg().wifi_credentials();
    if (creds.enable().read(fd) == 0)
    {
        printf("WiFi: off.\n");
        return;
    }

    wifiSsid = creds.ssid().read(fd);
    wifiPassword = creds.password().read(fd);
    wifiHostname = creds.hostname().read(fd);

    if (wifiSsid.empty())
    {
        printf("WiFi: on, but no network name is set.\n");
        return;
    }

    printf("WiFi: joining \"%s\".\n", wifiSsid.c_str());
    wifiManager.reset(new Esp32WiFiManager(wifiSsid.c_str(),
        wifiPassword.c_str(), &stack, cfg.seg().wifi(), WIFI_MODE_STA,
        Esp32WiFiManager::CONN_MODE_UPLINK_ONLY,
        wifiHostname.empty() ? "femtolcc-" : wifiHostname.c_str()));
}

extern "C" void app_main()
{
    printf("\nFemtoLCC — Uncommon Models, OpenMRN firmware "
           FEMTOLCC_OPENMRN_VERSION "\n");
    // The node ID is derived from the chip's MAC, so it is worth printing:
    // it is what a configuration tool will show, and what the default event
    // IDs were built from.
    printf("Node ID: %02X.%02X.%02X.%02X.%02X.%02X\n",
        (unsigned)((NODE_ID >> 40) & 0xFF), (unsigned)((NODE_ID >> 32) & 0xFF),
        (unsigned)((NODE_ID >> 24) & 0xFF), (unsigned)((NODE_ID >> 16) & 0xFF),
        (unsigned)((NODE_ID >> 8) & 0xFF), (unsigned)(NODE_ID & 0xFF));
    Esp32SocInfo::print_soc_info();

    // WiFi keeps its calibration data in NVS.
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    mount_filesystem();

    // Render the CDI from the ConfigDef and register it with the stack.
    CDIXMLGenerator::create_config_descriptor_xml(cfg, CDI_FILENAME, &stack);

    // Create the settings file with its defaults if it is missing or stale.
    const int fd = stack.create_config_file_if_needed(
        cfg.seg().internal_config(), openlcb::CANONICAL_VERSION,
        openlcb::CONFIG_FILE_SIZE);

    // Bring the I2C bus, the expander and the drivers up before the stack
    // starts, so the first configuration load has working hardware to talk to.
    controller.hw_init();

    start_wifi(fd);

    // Live control, memory space 0xE0: how a dispatcher such as olcbweb drives
    // the blocks moment to moment. Registered after the stack's own spaces so
    // it is served by the same Memory Configuration handler.
    stack.memory_config_handler()->registry()->insert(
        stack.node(), LiveControl::SPACE, controller.live_control());

    // Module storage, memory space 0xE1: the module's part of the track plan,
    // held for a dispatcher. Needs the file system, which is mounted above.
    if (controller.module_store()->begin())
    {
        stack.memory_config_handler()->registry()->insert(
            stack.node(), ModuleStore::SPACE, controller.module_store());
    }
    else
    {
        printf("module storage: no memory for the buffer; space 0xE1 absent\n");
    }

    // Make the WiFi password write-only. The stack registered its own space for
    // 0xFD when the settings file was created; this replaces that registration
    // with a wrapper around it.
    {
        const auto pw = cfg.seg().wifi_credentials().password();
        openlcb::MemorySpace *configSpace =
            stack.memory_config_handler()->registry()->lookup(
                stack.node(), openlcb::MemoryConfigDefs::SPACE_CONFIG);
        if (configSpace)
        {
            stack.memory_config_handler()->registry()->insert(stack.node(),
                openlcb::MemoryConfigDefs::SPACE_CONFIG,
                new WriteOnlyPasswordSpace(
                    configSpace, pw.offset(), pw.size()));
        }
        else
        {
            printf("configuration space not found; the WiFi password stays "
                   "readable\n");
        }
    }

    // Bring the CAN controller up and hand it to the stack. The TWAI driver is
    // read through the VFS with ::select().
    twai.hw_init();
    stack.add_can_port_select("/dev/twai/twai0");

    // The USB port as a GridConnect bridge, so the board works as an LCC
    // adapter for JMRI. OpenMRN opens the device O_RDWR and owns it, which is
    // why the console is not there too: add_gridconnect_port() gives the port
    // over entirely. The console is UART0 on the J1 header instead, and the
    // logging that would otherwise interleave with GridConnect frames is
    // silenced in sdkconfig.defaults.esp32c6.
#if defined(CONFIG_SOC_USB_SERIAL_JTAG_SUPPORTED)
    // ESP-IDF registers /dev/usbserjtag only when the USB Serial/JTAG port is
    // a console, and here it deliberately is not, so nothing would create the
    // device: the open below then fails and OpenMRN's HASSERT reboots the
    // board, over and over. Register it here instead, backed by the driver so
    // reads block - create_gc_port_for_can_hub() defaults to blocking reads on
    // threads of its own rather than select(), which this device does not
    // support anyway.
    usb_serial_jtag_driver_config_t usbCfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&usbCfg));
    // ESP-IDF registers the device itself when the port is a console, which it
    // is here only so that the VFS is compiled in at all - the logs it would
    // otherwise interleave with GridConnect are switched off in
    // sdkconfig.defaults.esp32c6. Registering twice is not an error worth
    // aborting over, so take either answer.
    const esp_err_t usbVfs = esp_vfs_dev_usb_serial_jtag_register();
    if (usbVfs != ESP_OK && usbVfs != ESP_ERR_INVALID_STATE)
    {
        ESP_ERROR_CHECK(usbVfs);
    }
    esp_vfs_usb_serial_jtag_use_driver();
    stack.add_gridconnect_port("/dev/usbserjtag");
#else
    // The fallback target has no USB Serial/JTAG. add_gridconnect_port() opens
    // the device and asserts on failure, so on a SoC without one it would abort
    // at boot rather than simply lack the bridge.
    printf("no USB Serial/JTAG on this target; the GridConnect bridge is off\n");
#endif

    // Never returns: the OpenMRN executor takes over this task.
    stack.loop_executor();
}
