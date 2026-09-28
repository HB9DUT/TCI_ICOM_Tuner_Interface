#pragma once

#include <cstdint>

// Status-LED mit Blinkmustern (Zyklus 2 s, 20 Schritte à 100 ms).
class StatusLed {
public:
    enum class Pattern : uint8_t {
        NoWifi,       // kurzer Blitz alle 2 s
        AccessPoint,  // Doppelblitz: Konfigurations-Access-Point aktiv
        WifiOnly,     // langsames Blinken: WLAN ok, TCI nicht bereit
        Ready,        // dauernd an: TCI bereit
        Tuning,       // schnelles Blinken: Abstimmung läuft
        Error,        // Dreifachblitz: letzte Abstimmung fehlgeschlagen
    };

    void begin();
    void set(Pattern p) { pattern_ = p; }
    void update();

private:
    Pattern pattern_ = Pattern::NoWifi;
    int8_t lastLevel_ = -1;
};
