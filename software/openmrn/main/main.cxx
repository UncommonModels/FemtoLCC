// FemtoLCC — OpenMRN firmware.
//
// Brings up an OpenLCB node on the FemtoLCC board using the full OpenMRN stack
// as an ESP-IDF component. The alternative firmware, in ../FemtoLCC, does the
// same job on AOLCB; only one of the two can be on the board at a time.
//
//   Uncommon Models — https://uncommonmodels.com

#include <memory>
#include <stdio.h>
#include <string.h>
#include <string>

#include "esp_spiffs.h"
#include "nvs_flash.h"

#include "freertos_drivers/esp32/Esp32HardwareTwai.hxx"
#include "freertos_drivers/esp32/Esp32SocInfo.hxx"
#include "freertos_drivers/esp32/Esp32WiFiManager.hxx"
#include "openlcb/SimpleStack.hxx"
#include "CDIXMLGenerator.hxx"

#include "FemtoController.hxx"
#include "board.h"
#include "config.hxx"

// ---------------------------------------------------------------------------
// Node identity
// ---------------------------------------------------------------------------

/// The same node ID as the AOLCB firmware. Only one firmware is on the board at
/// a time, so they never collide; the SNIP software version tells them apart.
static constexpr uint64_t NODE_ID = UINT64_C(0x020157000001);

namespace openlcb
{
/// Node identification, returned by Simple Node Information and written into
/// the generated CDI.
///
/// Manufacturer, model and hardware version match the AOLCB firmware — which
/// calls setSnip({"Uncommon Models", "FemtoLCC", "v1", ...}) — so the board is
/// the same product to a configuration tool. The software version string is
/// what tells the two firmwares apart.
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
/// credentials up front. Changing them therefore needs a restart, which is also
/// true of the AOLCB firmware.
/// Esp32WiFiManager::CONN_MODE_UPLINK_BIT is private, so the default — connect
/// out to a hub rather than being one — is spelled out here. Whatever is passed
/// is overridden by the CDI's "connection mode" field at the first
/// configuration load, so this only decides what happens before that.
static constexpr uint8_t CONN_MODE_UPLINK = 1;

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
        CONN_MODE_UPLINK,
        wifiHostname.empty() ? "femtolcc-" : wifiHostname.c_str()));
}

extern "C" void app_main()
{
    printf("\nFemtoLCC — Uncommon Models, OpenMRN firmware "
           FEMTOLCC_OPENMRN_VERSION "\n");
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

    // Bring the CAN controller up and hand it to the stack. The TWAI driver is
    // read through the VFS with ::select().
    twai.hw_init();
    stack.add_can_port_select("/dev/twai/twai0");

    // Never returns: the OpenMRN executor takes over this task.
    stack.loop_executor();
}
