#pragma once

#include <cstdint>
#include <string>

// Persistente Einstellungen (NVS), änderbar über die Weboberfläche.
struct Settings {
    std::string wifiSsid;
    std::string wifiPass;
    std::string hostname = "tci-tuner";
    std::string webPass;             // leer = Weboberfläche ohne Passwort

    std::string tciHost;
    uint16_t tciPort = 40001;        // Standard-Port des TCI-Servers in ExpertSDR3
    int8_t tuneTrx = -1;             // -1 = auf TUNE jedes Transceivers reagieren

    bool keyActiveHigh = true;       // KEY-Eingang aktiv bei HIGH (invertierender Transistor im Interface)
    uint16_t startHoldMs = 250;      // START nach Aktivierung von KEY noch so lange halten (AH-4: ca. 250 ms)
    uint16_t keyWaitMs = 2000;       // max. Zeit ab Start, bis der Tuner KEY aktiviert (AH-4: ca. 300 ms)
    uint16_t tuneTimeoutMs = 20000;  // max. Tune-Dauer ab Start, danach Träger aus
    uint16_t swrSettleMs = 300;      // Träger nach Tune-Ende halten, um SWR zu messen (0 = aus)
    float swrMax = 2.0f;             // max. zulässiges SWR nach dem Tunen (0 = keine Prüfung)

    void load();
    void save() const;
    void sanitize();
};
