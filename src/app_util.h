#pragma once

#include <cstdint>
#include <mutex>

#include "esp_timer.h"

// Millisekunden seit Start (läuft nach ca. 49 Tagen über; Differenzen bleiben korrekt)
inline uint32_t millis() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

// Schützt den Anwendungszustand zwischen Hauptschleife und HTTP-Server-Task.
inline std::recursive_mutex& appMutex() {
    static std::recursive_mutex m;
    return m;
}
