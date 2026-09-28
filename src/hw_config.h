#pragma once

#include "driver/gpio.h"

// Hardware-Belegung. Die Pegel beziehen sich auf den ESP32-Pin; die Anpassung
// an die AH-4-Leitungen (Open-Collector, ca. 8 V) übernimmt die Interface-Schaltung.
namespace hw {

constexpr gpio_num_t PIN_LED = GPIO_NUM_2;
constexpr int LED_ON = 1;

// START (ESP32 -> Tuner): aktiv = Transistor zieht die START-Leitung auf GND
constexpr gpio_num_t PIN_ATU_START = GPIO_NUM_27;
constexpr int START_ACTIVE = 1;

// KEY (Tuner -> ESP32): aktiv = Tuner fordert Träger an bzw. stimmt ab.
// Der aktive Pegel ist einstellbar (Settings::keyActiveHigh), weil er von der
// Interface-Schaltung abhängt (direkt/Diode: LOW, invertierender Transistor: HIGH).
constexpr gpio_num_t PIN_ATU_KEY = GPIO_NUM_26;

}  // namespace hw
