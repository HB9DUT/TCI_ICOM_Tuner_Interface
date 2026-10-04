#pragma once

#include "settings.h"
#include "tci_client.h"
#include "tuner.h"
#include "web_ui.h"

// Befehlskonsole über den seriellen Monitor (UART0, 115200 Baud) und Telnet
// (Port 23, eine Sitzung). Für die Inbetriebnahme ohne Umweg über den
// Access-Point und für die Fehlersuche: 'help' listet die Befehle.
//
// Telnet ist unverschlüsselt. Ist ein Passwort für die Weboberfläche gesetzt,
// fragt Telnet danach, weil 'tune' einen Träger auslöst.
namespace console {

void begin(Settings& cfg, TciClient& tci, Tuner& tuner, WebUi& web);

}  // namespace console
