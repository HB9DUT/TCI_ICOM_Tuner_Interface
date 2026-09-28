#include "network.h"

#include <atomic>
#include <cinttypes>
#include <cstring>
#include <vector>

#include "app_util.h"
#include "dns_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "mdns.h"

namespace {

constexpr const char* TAG = "wifi";

constexpr uint32_t AP_FALLBACK_MS = 30000;        // ohne WLAN-Verbindung so lange bis zum Access-Point
constexpr uint32_t AP_STA_RETRY_MS = 60000;       // WLAN-Neuversuch bei aktivem Access-Point
constexpr uint32_t AP_LINGER_MS = 60000;          // Access-Point nach WLAN-Verbindung noch offen lassen
constexpr uint32_t STA_RETRY_MS = 5000;           // WLAN-Neuversuch nach einem Fehlschlag
constexpr uint32_t STA_ATTEMPT_MAX_MS = 30000;    // laufenden Versuch spätestens dann abbrechen
constexpr uint32_t FAIL_LOG_INTERVAL_MS = 10000;  // gleiche Fehlerursache höchstens so oft melden
constexpr const char* AP_PASSWORD = "tci-tuner";
constexpr uint8_t AP_MAX_CLIENTS = 4;
constexpr uint8_t AP_CHANNEL = 1;
constexpr size_t SCAN_MAX_LISTED = 12;

const Settings* cfg = nullptr;
esp_netif_t* staNetif = nullptr;
esp_netif_t* apNetif = nullptr;
CaptiveDns dns;
std::string apName;
char portalUri[32];  // muss dauerhaft gültig bleiben (DHCP-Option 114)

bool apOn = false;
bool staWasConnected = false;
uint32_t staLostMs = 0;
uint32_t staConnectedMs = 0;
uint32_t staRetryMs = 0;

// Vom Event-Task gesetzt, in loop() ausgewertet
std::atomic<bool> gotIp{false};
std::atomic<uint32_t> staIpAddr{0};
std::atomic<uint32_t> disconnectCount{0};
std::atomic<uint8_t> disconnectReason{0};

uint32_t handledDisconnects = 0;
uint32_t failedAttempts = 0;
uint32_t failedAttemptsAtConnect = 0;
uint8_t loggedReason = 0;
uint32_t loggedReasonMs = 0;
bool scanPending = false;
bool scanDone = false;

const char* reasonName(uint8_t reason) {
    switch (reason) {
        case WIFI_REASON_AUTH_EXPIRE: return "AUTH_EXPIRE";
        case WIFI_REASON_AUTH_LEAVE: return "AUTH_LEAVE";
        case WIFI_REASON_ASSOC_LEAVE: return "ASSOC_LEAVE";
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT: return "4WAY_HANDSHAKE_TIMEOUT";
        case WIFI_REASON_BEACON_TIMEOUT: return "BEACON_TIMEOUT";
        case WIFI_REASON_NO_AP_FOUND: return "NO_AP_FOUND";
        case WIFI_REASON_AUTH_FAIL: return "AUTH_FAIL";
        case WIFI_REASON_ASSOC_FAIL: return "ASSOC_FAIL";
        case WIFI_REASON_HANDSHAKE_TIMEOUT: return "HANDSHAKE_TIMEOUT";
        case WIFI_REASON_CONNECTION_FAIL: return "CONNECTION_FAIL";
        case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY: return "NO_AP_FOUND_W_COMPATIBLE_SECURITY";
        case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD: return "NO_AP_FOUND_IN_AUTHMODE_THRESHOLD";
        default: return "?";
    }
}

const char* reasonHint(uint8_t reason) {
    switch (reason) {
        case WIFI_REASON_NO_AP_FOUND:
            return "SSID nicht gefunden (Name falsch, ausser Reichweite oder nur 5 GHz)";
        case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
        case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
            return "Verschlüsselung des WLANs nicht unterstützt";
        case WIFI_REASON_AUTH_FAIL:
        case WIFI_REASON_AUTH_EXPIRE:
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_HANDSHAKE_TIMEOUT:
            return "Anmeldung fehlgeschlagen (Passwort oder WPA3-Problem)";
        case WIFI_REASON_ASSOC_FAIL:
            return "Access-Point lehnt die Verbindung ab";
        case WIFI_REASON_BEACON_TIMEOUT:
            return "Signal verloren";
        default:
            return "Verbindung getrennt";
    }
}

const char* authName(wifi_auth_mode_t mode) {
    switch (mode) {
        case WIFI_AUTH_OPEN: return "offen";
        case WIFI_AUTH_WEP: return "WEP";
        case WIFI_AUTH_WPA_PSK: return "WPA";
        case WIFI_AUTH_WPA2_PSK: return "WPA2";
        case WIFI_AUTH_WPA_WPA2_PSK: return "WPA/WPA2";
        case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2-Enterprise";
        case WIFI_AUTH_WPA3_PSK: return "WPA3";
        case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/WPA3";
        default: return "unbekannt";
    }
}

std::string ipToString(uint32_t ip) {
    esp_ip4_addr_t addr = {ip};
    char buf[16];
    snprintf(buf, sizeof(buf), IPSTR, IP2STR(&addr));
    return buf;
}

void onEvent(void*, esp_event_base_t base, int32_t id, void* data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        gotIp = false;
        disconnectReason = static_cast<const wifi_event_sta_disconnected_t*>(data)->reason;
        ++disconnectCount;
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        staIpAddr = static_cast<const ip_event_got_ip_t*>(data)->ip_info.ip.addr;
        gotIp = true;
    } else if (base == IP_EVENT && id == IP_EVENT_STA_LOST_IP) {
        gotIp = false;
    }
}

void logDisconnects(uint32_t now) {
    const uint32_t count = disconnectCount;
    if (count == handledDisconnects) return;
    handledDisconnects = count;
    const uint8_t reason = disconnectReason;
    if (reason == WIFI_REASON_ASSOC_LEAVE) return;  // selbst ausgelöste Trennung
    ++failedAttempts;
    if (!scanDone) scanPending = true;
    if (reason == loggedReason && now - loggedReasonMs < FAIL_LOG_INTERVAL_MS) return;
    loggedReason = reason;
    loggedReasonMs = now;
    ESP_LOGW(TAG, "%s (Grund %u, %s, %" PRIu32 ". Fehlversuch)", reasonHint(reason), reason, reasonName(reason),
             failedAttempts);
}

void connectSta() {
    staRetryMs = millis();
    failedAttemptsAtConnect = failedAttempts;
    ESP_LOGI(TAG, "verbinde mit \"%s\" (Passwort: %u Zeichen)", cfg->wifiSsid.c_str(),
             static_cast<unsigned>(cfg->wifiPass.size()));

    wifi_config_t conf = {};
    strncpy(reinterpret_cast<char*>(conf.sta.ssid), cfg->wifiSsid.c_str(), sizeof(conf.sta.ssid));
    strncpy(reinterpret_cast<char*>(conf.sta.password), cfg->wifiPass.c_str(), sizeof(conf.sta.password));
    conf.sta.threshold.authmode = cfg->wifiPass.empty() ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    conf.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    conf.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    conf.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    esp_wifi_disconnect();
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &conf);
    if (err == ESP_OK) err = esp_wifi_connect();
    if (err != ESP_OK) ESP_LOGE(TAG, "Verbindungsaufbau fehlgeschlagen: %s", esp_err_to_name(err));
}

// Einmalige Diagnose nach dem ersten Fehlschlag: Ist das WLAN sichtbar, und wie ist es verschlüsselt?
void scanDiagnose() {
    scanPending = false;
    scanDone = true;
    esp_wifi_disconnect();  // laufende Verbindungsversuche stoppen, sonst schlägt der Scan fehl
    ESP_LOGI(TAG, "Scan: suche \"%s\"...", cfg->wifiSsid.c_str());
    if (esp_wifi_scan_start(nullptr, true) != ESP_OK) {
        ESP_LOGW(TAG, "Scan fehlgeschlagen");
        return;
    }
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    std::vector<wifi_ap_record_t> aps(n);
    esp_wifi_scan_get_ap_records(&n, aps.data());
    aps.resize(n);

    bool found = false;
    for (const wifi_ap_record_t& ap : aps) {
        if (cfg->wifiSsid != reinterpret_cast<const char*>(ap.ssid)) continue;
        found = true;
        ESP_LOGI(TAG, "Scan: gefunden, Kanal %u, %d dBm, %s, BSSID " MACSTR, ap.primary, ap.rssi,
                 authName(ap.authmode), MAC2STR(ap.bssid));
    }
    if (!found) {
        ESP_LOGW(TAG, "Scan: \"%s\" nicht gefunden, %u Netze sichtbar:", cfg->wifiSsid.c_str(), n);
        for (size_t i = 0; i < aps.size() && i < SCAN_MAX_LISTED; ++i) {
            ESP_LOGW(TAG, "  \"%s\" Kanal %u, %d dBm, %s", reinterpret_cast<const char*>(aps[i].ssid),
                     aps[i].primary, aps[i].rssi, authName(aps[i].authmode));
        }
    }
}

void startAp() {
    esp_wifi_set_mode(WIFI_MODE_APSTA);

    wifi_config_t conf = {};
    strncpy(reinterpret_cast<char*>(conf.ap.ssid), apName.c_str(), sizeof(conf.ap.ssid));
    conf.ap.ssid_len = apName.size();
    strncpy(reinterpret_cast<char*>(conf.ap.password), AP_PASSWORD, sizeof(conf.ap.password));
    conf.ap.authmode = WIFI_AUTH_WPA2_PSK;
    conf.ap.channel = AP_CHANNEL;
    conf.ap.max_connection = AP_MAX_CLIENTS;
    esp_wifi_set_config(WIFI_IF_AP, &conf);

    // DHCP-Option 114: Geräte finden die Konfigurationsseite ohne Umleitungstricks
    const uint32_t ip = net::apIp();
    snprintf(portalUri, sizeof(portalUri), "http://%s/", ipToString(ip).c_str());
    esp_netif_dhcps_stop(apNetif);
    esp_netif_dhcps_option(apNetif, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI, portalUri, strlen(portalUri));
    esp_netif_dhcps_start(apNetif);

    dns.start(ip);
    apOn = true;
    ESP_LOGW(TAG, "Access-Point \"%s\" (Passwort \"%s\"), %s", apName.c_str(), AP_PASSWORD, portalUri);
}

void stopAp() {
    dns.stop();
    esp_wifi_set_mode(WIFI_MODE_STA);
    apOn = false;
    ESP_LOGI(TAG, "Access-Point beendet");
}

uint8_t apClientCount() {
    wifi_sta_list_t list = {};
    return esp_wifi_ap_get_sta_list(&list) == ESP_OK ? list.num : 0;
}

}  // namespace

void net::begin(const Settings& settings) {
    cfg = &settings;

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char name[24];
    snprintf(name, sizeof(name), "TCI-Tuner-%02X%02X", mac[4], mac[5]);
    apName = name;

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    staNetif = esp_netif_create_default_wifi_sta();
    apNetif = esp_netif_create_default_wifi_ap();
    esp_netif_set_hostname(staNetif, cfg->hostname.c_str());

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    esp_wifi_set_storage(WIFI_STORAGE_RAM);  // Zugangsdaten nur in den eigenen Einstellungen
    esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, onEvent, nullptr, nullptr);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, onEvent, nullptr, nullptr);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_LOST_IP, onEvent, nullptr, nullptr);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(WIFI_PS_NONE);  // Modem-Sleep erhöht die Latenz der TCI-Verbindung

    if (mdns_init() == ESP_OK) {
        mdns_hostname_set(cfg->hostname.c_str());
        mdns_instance_name_set("TCI ICOM Tuner Interface");
        mdns_service_add(nullptr, "_http", "_tcp", 80, nullptr, 0);
    }

    staLostMs = millis();
    if (cfg->wifiSsid.empty()) {
        ESP_LOGW(TAG, "keine SSID konfiguriert");
        startAp();
    } else {
        connectSta();
    }
}

void net::loop() {
    const uint32_t now = millis();
    const bool sta = gotIp;
    logDisconnects(now);

    if (sta != staWasConnected) {
        staWasConnected = sta;
        if (sta) {
            staConnectedMs = now;
            failedAttempts = failedAttemptsAtConnect = 0;
            ESP_LOGI(TAG, "verbunden, IP %s, RSSI %d dBm", staIp().c_str(), staRssi());
            ESP_LOGI(TAG, "erreichbar als http://%s.local/", cfg->hostname.c_str());
        } else {
            staLostMs = now;
            ESP_LOGW(TAG, "Verbindung getrennt");
        }
    }
    if (sta || cfg->wifiSsid.empty()) {
        if (apOn && sta && apClientCount() == 0 && now - staConnectedMs >= AP_LINGER_MS) stopAp();
        return;
    }

    const uint32_t sinceAttempt = now - staRetryMs;
    if (apOn) {
        // Verbindungsversuche stören den Access-Point; nur ohne verbundene Geräte wiederholen
        if (apClientCount() == 0 && sinceAttempt >= AP_STA_RETRY_MS) connectSta();
    } else if (now - staLostMs >= AP_FALLBACK_MS) {
        ESP_LOGW(TAG, "keine Verbindung nach %" PRIu32 " s", AP_FALLBACK_MS / 1000);
        startAp();
        staRetryMs = now;
    } else if (scanPending) {
        scanDiagnose();
        connectSta();
    } else {
        // Nicht in einen laufenden Versuch hineinfunken (WPA3/SAE kann einige Sekunden dauern)
        const bool attemptFailed = failedAttempts != failedAttemptsAtConnect;
        if ((attemptFailed && sinceAttempt >= STA_RETRY_MS) || sinceAttempt >= STA_ATTEMPT_MAX_MS) connectSta();
    }
}

bool net::staConnected() { return gotIp; }

std::string net::staIp() { return gotIp ? ipToString(staIpAddr) : std::string(); }

std::string net::staSsid() {
    wifi_ap_record_t info;
    if (!gotIp || esp_wifi_sta_get_ap_info(&info) != ESP_OK) return {};
    return reinterpret_cast<const char*>(info.ssid);
}

int net::staRssi() {
    wifi_ap_record_t info;
    return (gotIp && esp_wifi_sta_get_ap_info(&info) == ESP_OK) ? info.rssi : 0;
}

bool net::apActive() { return apOn; }

const std::string& net::apSsid() { return apName; }

uint32_t net::apIp() {
    esp_netif_ip_info_t info = {};
    if (apNetif) esp_netif_get_ip_info(apNetif, &info);
    return info.ip.addr;
}
