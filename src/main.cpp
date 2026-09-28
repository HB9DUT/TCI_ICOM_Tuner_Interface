// TCI ICOM Tuner Interface – steuert einen Tuner mit ICOM-AH-4-Schnittstelle
// (z.B. ICOM AH-4, Stockcorner) anhand der TUNE-Befehle von ExpertSDR3 (TCI).
//
// Die Anwendungslogik läuft in einer Hauptschleife (5-ms-Takt). Der WebSocket-Client
// liefert seine Ereignisse über eine Queue; der HTTP-Server sperrt appMutex().

#include "app_util.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "network.h"
#include "nvs_flash.h"
#include "settings.h"
#include "status_led.h"
#include "tci_client.h"
#include "tuner.h"
#include "web_ui.h"

namespace {

constexpr const char* TAG = "main";
constexpr uint32_t LOOP_PERIOD_MS = 5;
constexpr uint32_t ERROR_DISPLAY_MS = 30000;

Settings settings;
TciClient tci;
Tuner tuner(settings, tci);
StatusLed led;
WebUi web(settings, tci, tuner);

StatusLed::Pattern ledPattern() {
    if (tuner.busy()) return StatusLed::Pattern::Tuning;
    if (tuner.lastFailed() && millis() - tuner.lastFinishedMs() < ERROR_DISPLAY_MS) return StatusLed::Pattern::Error;
    if (tci.ready()) return StatusLed::Pattern::Ready;
    if (net::staConnected()) return StatusLed::Pattern::WifiOnly;
    if (net::apActive()) return StatusLed::Pattern::AccessPoint;
    return StatusLed::Pattern::NoWifi;
}

void initNvs() {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS wird neu initialisiert");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

}  // namespace

extern "C" void app_main() {
    tuner.begin();  // START-Leitung sofort in den inaktiven Zustand
    led.begin();

    ESP_LOGI(TAG, "TCI ICOM Tuner Interface, Firmware %s", esp_app_get_description()->version);
    initNvs();
    settings.load();

    tci.onReady = [](bool ready) { tuner.onTciReady(ready); };
    tci.onTune = [](int trx, bool on) { tuner.onTuneEvent(trx, on); };
    tci.onTxSensors = [](int trx, float swr) { tuner.onTxSensors(trx, swr); };

    net::begin(settings);
    tci.configure(settings.tciHost, settings.tciPort);
    web.begin();

    for (;;) {
        {
            std::lock_guard<std::recursive_mutex> lock(appMutex());
            net::loop();
            tci.loop(net::staConnected());
            tuner.update();
            led.set(ledPattern());
            led.update();
        }
        if (web.rebootDue()) {
            ESP_LOGI(TAG, "Neustart");
            esp_restart();
        }
        vTaskDelay(pdMS_TO_TICKS(LOOP_PERIOD_MS));
    }
}
