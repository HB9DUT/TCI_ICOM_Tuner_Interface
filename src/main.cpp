// TCI to ICOM Tuner Interface – steuert einen Tuner mit ICOM-AH-4-Schnittstelle
// (z.B. ICOM AH-4, Stockcorner) anhand der TUNE-Befehle eines SDR-Programms (TCI).
//
// Tasks:
//   tuner  (Priorität 10, Core 1) Zustandsmaschine, START/KEY; Befehle nur über Queue
//   tci    (Priorität 6)          Verbindung, Empfang und Senden über TCI
//   httpd  (Priorität 5)          Weboberfläche; sperrt appMutex() für Einstellungen
//   main   (Priorität 1)          WLAN, Status-LED, Neustart
// Die Weboberfläche kann den Tuner damit nicht aufhalten.

#include "app_util.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
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
Tuner tuner(tci);
StatusLed led;
WebUi web(settings, tci, tuner);

StatusLed::Pattern ledPattern() {
    if (tuner.busy()) return StatusLed::Pattern::Tuning;
    const Tuner::Snapshot t = tuner.snapshot();
    if (t.lastFailed && millis() - t.lastFinishedMs < ERROR_DISPLAY_MS) return StatusLed::Pattern::Error;
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

// Nach einem Update startet die neue Firmware im Prüfzustand. Erreicht sie den
// Webserver, gilt sie als gültig; sonst kehrt der Bootloader zur alten zurück.
void confirmFirmware() {
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "neue Firmware bestätigt");
    }
}

}  // namespace

extern "C" void app_main() {
    tuner.initOutput();  // START-Leitung sofort in den inaktiven Zustand
    led.begin();

    ESP_LOGI(TAG, "TCI to ICOM Tuner Interface, Firmware %s", esp_app_get_description()->version);
    initNvs();
    settings.load();

    tci.onReady = [](bool ready) { tuner.onTciReady(ready); };
    tci.onTune = [](int trx, bool on, uint32_t freqHz, bool atConnect, bool carrierOn) {
        tuner.onTuneEvent(trx, on, freqHz, atConnect, carrierOn);
    };
    tci.onTxSensors = [](int trx, float swr) { tuner.onTxSensors(trx, swr); };
    tuner.start(Tuner::Config::from(settings));

    net::begin(settings);
    tci.configure(settings.tciHost, settings.tciPort);
    tci.startTask();
    if (web.begin()) confirmFirmware();

    bool rebootWaitLogged = false;
    for (;;) {
        {
            std::lock_guard<std::recursive_mutex> lock(appMutex());
            net::loop();
        }
        led.set(ledPattern());
        led.update();
        // Neustart erst, wenn der Tuner fertig ist: sonst bliebe der Träger im
        // SDR-Programm an, weil niemand mehr TUNE:false schickt.
        if (web.rebootDue()) {
            if (!tuner.busy()) {
                ESP_LOGI(TAG, "Neustart");
                esp_restart();
            }
            if (!rebootWaitLogged) ESP_LOGI(TAG, "Neustart wartet auf das Ende der Abstimmung");
            rebootWaitLogged = true;
        }
        vTaskDelay(pdMS_TO_TICKS(LOOP_PERIOD_MS));
    }
}
