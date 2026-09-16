// The node's connection to the layout: CAN, optionally WiFi, and GridConnect
// on the USB port.
//
// Everything the node sends goes to a Hub, which joins the CAN interface, a
// GridConnect-over-TCP transport and the USB bridge into one bus and bridges
// frames between them. So a JMRI connected over WiFi or USB sees the CAN bus
// too, and the node can run with no CAN bus at all.
//
// Over WiFi the board either acts as a hub - listening on the TCP port, and
// advertising itself over mDNS as an _openlcb-can._tcp service so JMRI and
// other tools can find it - or connects out to a hub elsewhere.
//
// WiFi settings are read once, at begin(). A change through the CDI takes
// effect after a reboot; the console's `wifi` command reconnects at once.
//
// (Not named Network.h: the ESP32 core has a library of that name, and a
// sketch file would shadow it.)

#pragma once

#include <stdint.h>
#include <AOLCB.h>
#include <Hub.h>
#include <GridConnectTcp.h>
#include "Config.h"

class Print;

class WifiLink {
public:
    explicit WifiLink(AOLCB::ESP32CANInterface& can);

    // One more interface to join to the bus - GridConnect on the USB port.
    // Call before begin().
    void addPort(AOLCB::Interface& port);

    // Join CAN and the configured TCP transport into the hub, start them, and
    // start joining WiFi if it is enabled. Never waits for WiFi. Returns false
    // if the CAN controller did not start; the rest runs regardless.
    bool begin(const WifiSettings& wifi);

    // The bus the node should run on.
    AOLCB::Interface& bus() { return hub_; }

    // Watches the WiFi connection and reports changes. Call every loop.
    void update();

    // Join a network now, with new credentials. Used by the console.
    void connect(const char* ssid, const char* password);

    // Leave the network and turn the radio off.
    void disable();

    // True if these settings are the ones running.
    bool running(const WifiSettings& wifi) const;

    void printStatus(Print& out) const;

private:
    void startWifi();

    AOLCB::ESP32CANInterface& can_;
    AOLCB::Hub hub_;
    AOLCB::GridConnectTcpServer* server_;
    AOLCB::GridConnectTcpClient* client_;
    AOLCB::Interface* extra_;

    // A private copy: the TCP client keeps a pointer to the host name, so it
    // must not change underneath it when the configuration is re-read.
    WifiSettings wifi_;
    bool radioOn_;
    bool connected_;
    uint32_t lastCheck_;
};
