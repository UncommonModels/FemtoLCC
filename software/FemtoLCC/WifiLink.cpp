#include "WifiLink.h"
#include <Arduino.h>
#include <WiFi.h>
#include <string.h>
#include "Notice.h"

// How often the WiFi state is looked at. Status is cheap, but there is no
// reason to ask every loop.
static const uint32_t CHECK_MS = 500;

WifiLink::WifiLink(AOLCB::ESP32CANInterface& can)
    : can_(can), server_(nullptr), client_(nullptr), extra_(nullptr), wifi_{},
      radioOn_(false), connected_(false), lastCheck_(0) {}

void WifiLink::addPort(AOLCB::Interface& port) {
    extra_ = &port;
}

bool WifiLink::begin(const WifiSettings& wifi) {
    wifi_ = wifi;

    AOLCB::Configuration config;
    const bool canOk = can_.begin(config);
    hub_.addPort(can_);

    // The TCP transport is created even with WiFi disabled. It does nothing
    // until there is a network, and it means `wifi <ssid> <password>` on the
    // console can bring the node onto WiFi without a reboot.
    if (wifi_.hubMode == HubMode::Server) {
        server_ = new AOLCB::GridConnectTcpServer(wifi_.hubPort);
        hub_.addPort(*server_);
        // Takes effect whenever the network comes up.
        server_->advertise(wifi_.hostname);
    } else if (wifi_.hubHost[0]) {
        client_ = new AOLCB::GridConnectTcpClient(wifi_.hubHost, wifi_.hubPort);
        hub_.addPort(*client_);
    }
    if (extra_) {
        hub_.addPort(*extra_);
    }
    hub_.begin(config);

    if (wifi_.enabled && wifi_.ssid[0]) {
        startWifi();
    }
    return canOk;
}

void WifiLink::startWifi() {
    // Credentials live in the node's configuration, not the WiFi driver's own
    // NVS copy.
    WiFi.persistent(false);
    // The hostname must be set before the station interface comes up.
    WiFi.setHostname(wifi_.hostname);
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(wifi_.ssid, wifi_.password[0] ? wifi_.password : nullptr);
    radioOn_ = true;
    connected_ = false;
    notice("WiFi: joining '%s' as %s\n", wifi_.ssid, wifi_.hostname);
}

void WifiLink::update() {
    if (!radioOn_ || millis() - lastCheck_ < CHECK_MS) {
        return;
    }
    lastCheck_ = millis();

    const bool now = WiFi.status() == WL_CONNECTED;
    if (now && !connected_) {
        notice("WiFi: connected, IP %s, %s.local\n",
               WiFi.localIP().toString().c_str(), wifi_.hostname);
        if (server_) {
            notice("LCC hub listening on port %u\n", wifi_.hubPort);
        } else if (client_) {
            notice("LCC: connecting to hub %s:%u\n", wifi_.hubHost, wifi_.hubPort);
        }
    } else if (!now && connected_) {
        notice("WiFi: connection lost, retrying\n");
    }
    connected_ = now;
}

void WifiLink::connect(const char* ssid, const char* password) {
    strncpy(wifi_.ssid, ssid, sizeof(wifi_.ssid) - 1);
    wifi_.ssid[sizeof(wifi_.ssid) - 1] = '\0';
    strncpy(wifi_.password, password, sizeof(wifi_.password) - 1);
    wifi_.password[sizeof(wifi_.password) - 1] = '\0';
    wifi_.enabled = true;

    if (radioOn_) {
        WiFi.disconnect();
    }
    startWifi();
    if (!server_ && !client_) {
        Serial.println(F("WiFi: no hub address configured, so nothing will connect over it"));
    }
}

void WifiLink::disable() {
    wifi_.enabled = false;
    if (radioOn_) {
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
    }
    radioOn_ = false;
    connected_ = false;
}

bool WifiLink::running(const WifiSettings& w) const {
    return w.enabled == wifi_.enabled && w.hubMode == wifi_.hubMode &&
           w.hubPort == wifi_.hubPort &&
           strcmp(w.ssid, wifi_.ssid) == 0 &&
           strcmp(w.password, wifi_.password) == 0 &&
           strcmp(w.hostname, wifi_.hostname) == 0 &&
           strcmp(w.hubHost, wifi_.hubHost) == 0;
}

void WifiLink::printStatus(Print& out) const {
    out.printf("WiFi %s, SSID '%s', hostname %s\n",
               wifi_.enabled ? "enabled" : "disabled", wifi_.ssid, wifi_.hostname);
    if (radioOn_) {
        if (WiFi.status() == WL_CONNECTED) {
            out.printf("  connected, IP %s, signal %d dBm\n",
                       WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());
        } else {
            out.printf("  not connected (status %d)\n", (int)WiFi.status());
        }
    }
    if (server_) {
        out.printf("  hub on port %u, %u client(s)\n", wifi_.hubPort, server_->clientCount());
    } else if (client_) {
        out.printf("  hub client to %s:%u, %s\n", wifi_.hubHost, wifi_.hubPort,
                   client_->connected() ? "connected" : "not connected");
    } else {
        out.println(F("  no hub: connect-to-hub mode with no address"));
    }
}
