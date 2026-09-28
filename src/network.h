#pragma once

#include <cstdint>
#include <string>

#include "settings.h"

// WLAN-Station mit Fallback auf einen Konfigurations-Access-Point (Captive Portal).
// begin()/loop() laufen in der Hauptschleife; die Abfragen sind aus jedem Task erlaubt.
namespace net {

void begin(const Settings& settings);
void loop();

bool staConnected();
std::string staIp();
std::string staSsid();
int staRssi();

bool apActive();
const std::string& apSsid();
uint32_t apIp();  // IPv4 in Netzwerk-Byte-Reihenfolge

}  // namespace net
