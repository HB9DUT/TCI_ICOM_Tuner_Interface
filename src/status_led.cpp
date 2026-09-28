#include "status_led.h"

#include "app_util.h"
#include "hw_config.h"

namespace {

constexpr uint32_t STEP_MS = 100;
constexpr uint32_t STEPS = 20;

// Bit n = LED an im Schritt n
uint32_t mask(StatusLed::Pattern p) {
    switch (p) {
        case StatusLed::Pattern::NoWifi: return 0b1;
        case StatusLed::Pattern::AccessPoint: return 0b101;
        case StatusLed::Pattern::WifiOnly: return 0x1F | (0x1F << 10);
        case StatusLed::Pattern::Ready: return 0xFFFFF;
        case StatusLed::Pattern::Tuning: return 0x55555;
        case StatusLed::Pattern::Error: return 0b10101;
    }
    return 0;
}

}  // namespace

void StatusLed::begin() {
    gpio_config_t out = {};
    out.pin_bit_mask = 1ULL << hw::PIN_LED;
    out.mode = GPIO_MODE_OUTPUT;
    gpio_config(&out);
    update();
}

void StatusLed::update() {
    const uint32_t step = (millis() / STEP_MS) % STEPS;
    const int8_t level = (mask(pattern_) >> step) & 1;
    if (level == lastLevel_) return;
    lastLevel_ = level;
    gpio_set_level(hw::PIN_LED, level ? hw::LED_ON : !hw::LED_ON);
}
