#include "settings.h"

#include <algorithm>
#include <cctype>
#include <cmath>

#include "esp_log.h"
#include "nvs.h"

// Schlüssel und Typen entsprechen der Arduino-Version (Preferences), damit
// gespeicherte Einstellungen nach dem Umstieg erhalten bleiben.

namespace {

constexpr const char* TAG = "settings";
constexpr const char* NS = "stockcorner";  // Name aus der ersten Version; beibehalten, damit Einstellungen erhalten bleiben
constexpr uint8_t CFG_VERSION = 1;

void getStr(nvs_handle_t h, const char* key, std::string& value) {
    size_t len = 0;
    if (nvs_get_str(h, key, nullptr, &len) != ESP_OK || len == 0) return;
    std::string s(len, '\0');
    if (nvs_get_str(h, key, s.data(), &len) != ESP_OK) return;
    s.resize(len - 1);  // abschliessende Null
    value = s;
}

template <typename T>
T clampValue(T v, T lo, T hi) {
    return std::min(std::max(v, lo), hi);
}

}  // namespace

void Settings::load() {
    nvs_handle_t h;
    uint8_t version = 0;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "keine gespeicherten Einstellungen, verwende Standardwerte");
        return;
    }
    if (nvs_get_u8(h, "cfg_ver", &version) == ESP_OK) {
        getStr(h, "wifi_ssid", wifiSsid);
        getStr(h, "wifi_pass", wifiPass);
        getStr(h, "hostname", hostname);
        getStr(h, "web_pass", webPass);
        getStr(h, "tci_host", tciHost);
        nvs_get_u16(h, "tci_port", &tciPort);
        nvs_get_i8(h, "tune_trx", &tuneTrx);
        uint8_t keyHigh = keyActiveHigh;
        if (nvs_get_u8(h, "key_act_high", &keyHigh) == ESP_OK) keyActiveHigh = keyHigh != 0;
        nvs_get_u16(h, "pulse_ms", &startHoldMs);
        nvs_get_u16(h, "key_wait_ms", &keyWaitMs);
        nvs_get_u16(h, "tune_tmo_ms", &tuneTimeoutMs);
        nvs_get_u16(h, "settle_ms", &swrSettleMs);
        float f;
        size_t len = sizeof(f);
        if (nvs_get_blob(h, "swr_max", &f, &len) == ESP_OK && len == sizeof(f)) swrMax = f;
    }
    nvs_close(h);
    sanitize();
}

void Settings::save() const {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "NVS kann nicht geöffnet werden");
        return;
    }
    nvs_set_str(h, "wifi_ssid", wifiSsid.c_str());
    nvs_set_str(h, "wifi_pass", wifiPass.c_str());
    nvs_set_str(h, "hostname", hostname.c_str());
    nvs_set_str(h, "web_pass", webPass.c_str());
    nvs_set_str(h, "tci_host", tciHost.c_str());
    nvs_set_u16(h, "tci_port", tciPort);
    nvs_set_i8(h, "tune_trx", tuneTrx);
    nvs_set_u8(h, "key_act_high", keyActiveHigh ? 1 : 0);
    nvs_set_u16(h, "pulse_ms", startHoldMs);
    nvs_set_u16(h, "key_wait_ms", keyWaitMs);
    nvs_set_u16(h, "tune_tmo_ms", tuneTimeoutMs);
    nvs_set_u16(h, "settle_ms", swrSettleMs);
    nvs_set_blob(h, "swr_max", &swrMax, sizeof(swrMax));
    nvs_set_u8(h, "cfg_ver", CFG_VERSION);
    const esp_err_t err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Einstellungen gespeichert");
    } else {
        ESP_LOGE(TAG, "Speichern fehlgeschlagen: %s", esp_err_to_name(err));
    }
}

void Settings::erase() {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_erase_all(h);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "Einstellungen gelöscht");
}

void Settings::sanitize() {
    if (tciPort == 0) tciPort = 40001;
    if (tuneTrx < -1 || tuneTrx > 7) tuneTrx = -1;

    startHoldMs = clampValue<uint16_t>(startHoldMs, 50, 2000);
    keyWaitMs = clampValue<uint16_t>(keyWaitMs, 500, 10000);
    tuneTimeoutMs = clampValue<uint16_t>(tuneTimeoutMs, keyWaitMs + 1000, 60000);
    swrSettleMs = clampValue<uint16_t>(swrSettleMs, 0, 2000);
    if (!(swrMax >= 1.0f)) swrMax = 0.0f;  // fängt auch NaN ab
    if (swrMax > 10.0f) swrMax = 10.0f;

    // Hostname: nur a-z, 0-9 und '-', wie für DHCP/mDNS erforderlich
    std::string h;
    for (char c : hostname) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (h.size() >= 32) break;
        if (std::isalnum(static_cast<unsigned char>(c)) || (c == '-' && !h.empty())) h += c;
    }
    hostname = h.empty() ? "tci-tuner" : h;
}
