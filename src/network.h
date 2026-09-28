#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "settings.h"

// WLAN-Station mit Fallback auf einen Konfigurations-Access-Point (Captive Portal).
// begin()/loop() laufen in der Hauptschleife; die Abfragen sind aus jedem Task erlaubt.
namespace net {

struct Network {
    std::string ssid;
    int rssi;
    uint8_t channel;
    bool secure;
};

void begin(const Settings& settings);
void loop();

bool staConnected();
std::string staIp();
std::string staSsid();
int staRssi();

bool apActive();
const std::string& apSsid();
uint32_t apIp();  // IPv4 in Netzwerk-Byte-Reihenfolge

// Sucht sichtbare WLANs (blockiert ca. 2 s; nicht mit gesperrtem appMutex aufrufen).
// Ergebnis ohne versteckte Netze, je SSID nur das stärkste, nach Signalstärke sortiert.
bool scan(std::vector<Network>& result);

}  // namespace net
