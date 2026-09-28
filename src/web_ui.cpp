#include "web_ui.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <string>

#include "app_util.h"
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "lwip/sockets.h"
#include "mbedtls/base64.h"
#include "network.h"

extern const char INDEX_HTML_START[] asm("_binary_index_html_start");
extern const char INDEX_HTML_END[] asm("_binary_index_html_end");

namespace {

constexpr const char* TAG = "web";
constexpr const char* WEB_USER = "admin";
constexpr uint32_t REBOOT_DELAY_MS = 1000;
constexpr size_t MAX_BODY = 2048;
constexpr size_t OTA_CHUNK = 4096;
constexpr int OTA_MAX_TIMEOUTS = 3;

WebUi* self(httpd_req_t* req) { return static_cast<WebUi*>(req->user_ctx); }

esp_err_t sendJson(httpd_req_t* req, cJSON* root) {
    char* text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!text) return httpd_resp_send_500(req);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    const esp_err_t err = httpd_resp_sendstr(req, text);
    cJSON_free(text);
    return err;
}

void addFloat(cJSON* obj, const char* name, float v, int decimals) {
    if (std::isnan(v)) {
        cJSON_AddNullToObject(obj, name);
    } else {
        const double scale = std::pow(10.0, decimals);
        cJSON_AddNumberToObject(obj, name, std::round(v * scale) / scale);
    }
}

int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string urlDecode(const char* s) {
    std::string out;
    for (; *s; ++s) {
        if (*s == '+') {
            out += ' ';
        } else if (*s == '%' && hexValue(s[1]) >= 0 && hexValue(s[2]) >= 0) {
            out += static_cast<char>(hexValue(s[1]) << 4 | hexValue(s[2]));
            s += 2;
        } else {
            out += *s;
        }
    }
    return out;
}

// Liest ein Feld aus einem application/x-www-form-urlencoded-Body.
bool formValue(const std::string& body, const char* key, std::string& value) {
    char buf[256];
    if (httpd_query_key_value(body.c_str(), key, buf, sizeof(buf)) != ESP_OK) return false;
    value = urlDecode(buf);
    return true;
}

// Leeres oder fehlendes Feld = unverändert; Werte ausserhalb des Typs werden begrenzt
template <typename T>
void formNumber(const std::string& body, const char* key, T& field) {
    std::string v;
    if (!formValue(body, key, v) || v.empty()) return;
    long n = strtol(v.c_str(), nullptr, 10);
    n = std::max<long>(n, std::numeric_limits<T>::min());
    n = std::min<long>(n, std::numeric_limits<T>::max());
    field = static_cast<T>(n);
}

void trim(std::string& s) {
    const size_t first = s.find_first_not_of(" \t\r\n");
    const size_t last = s.find_last_not_of(" \t\r\n");
    s = first == std::string::npos ? std::string() : s.substr(first, last - first + 1);
}

// Lokale IPv4-Adresse der Verbindung (über welches Interface kam die Anfrage?)
uint32_t localAddress(httpd_req_t* req) {
    sockaddr_storage addr = {};
    socklen_t len = sizeof(addr);
    if (getsockname(httpd_req_to_sockfd(req), reinterpret_cast<sockaddr*>(&addr), &len) != 0) return 0;
    if (addr.ss_family == AF_INET) return reinterpret_cast<sockaddr_in*>(&addr)->sin_addr.s_addr;
    if (addr.ss_family == AF_INET6) return reinterpret_cast<sockaddr_in6*>(&addr)->sin6_addr.un.u32_addr[3];
    return 0;
}

// Liest genau len Bytes des Bodys; false bei Abbruch der Verbindung.
bool recvExact(httpd_req_t* req, char* buf, size_t len) {
    int timeouts = 0;
    for (size_t got = 0; got < len;) {
        const int n = httpd_req_recv(req, buf + got, len - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts <= OTA_MAX_TIMEOUTS) continue;
        if (n <= 0) return false;
        timeouts = 0;
        got += n;
    }
    return true;
}

// Prüft anhand des App-Deskriptors im ersten Block, ob die Datei eine Firmware
// dieses Projekts ist (und nicht etwa die eines anderen ESP32-Geräts).
const char* checkImage(const char* data, size_t len) {
    constexpr size_t offset = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t);
    if (len < offset + sizeof(esp_app_desc_t) || static_cast<uint8_t>(data[0]) != ESP_IMAGE_HEADER_MAGIC) {
        return "not_firmware";
    }
    esp_app_desc_t desc;
    memcpy(&desc, data + offset, sizeof(desc));
    if (desc.magic_word != ESP_APP_DESC_MAGIC_WORD) return "not_firmware";
    if (strncmp(desc.project_name, esp_app_get_description()->project_name, sizeof(desc.project_name)) != 0) {
        return "wrong_project";
    }
    ESP_LOGI(TAG, "Update auf Firmware %s", desc.version);
    return nullptr;
}

// Schreibt den Body in die freie App-Partition und macht sie zur Startpartition.
// Liefert nullptr bei Erfolg, sonst eine Fehlerkennung (von der Weboberfläche übersetzt).
const char* receiveFirmware(httpd_req_t* req) {
    const esp_partition_t* part = esp_ota_get_next_update_partition(nullptr);
    if (!part) return "no_partition";
    if (req->content_len == 0 || req->content_len > part->size) return "bad_size";

    std::unique_ptr<char[]> buf(new (std::nothrow) char[OTA_CHUNK]);
    if (!buf) return "no_memory";
    esp_ota_handle_t ota = 0;
    if (esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &ota) != ESP_OK) return "begin_failed";

    const char* error = nullptr;
    for (size_t done = 0; done < req->content_len && !error;) {
        const size_t len = std::min(OTA_CHUNK, req->content_len - done);
        if (!recvExact(req, buf.get(), len)) {
            error = "transfer_aborted";
            break;
        }
        if (done == 0) error = checkImage(buf.get(), len);
        if (!error && esp_ota_write(ota, buf.get(), len) != ESP_OK) error = "write_failed";
        done += len;
    }
    if (error) {
        esp_ota_abort(ota);
        return error;
    }
    if (esp_ota_end(ota) != ESP_OK) return "corrupt";
    if (esp_ota_set_boot_partition(part) != ESP_OK) return "boot_failed";
    return nullptr;
}

}  // namespace

bool WebUi::begin() {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 6144;
    config.lru_purge_enable = true;  // Captive-Portal-Prüfungen öffnen viele Verbindungen
    if (httpd_start(&server_, &config) != ESP_OK) {
        ESP_LOGE(TAG, "HTTP-Server konnte nicht gestartet werden");
        return false;
    }

    const httpd_uri_t routes[] = {
        {"/", HTTP_GET, handleIndex, this},
        {"/api/status", HTTP_GET, handleStatus, this},
        {"/api/settings", HTTP_GET, handleGetSettings, this},
        {"/api/settings", HTTP_POST, handlePostSettings, this},
        {"/api/reboot", HTTP_POST, handleReboot, this},
        {"/api/update", HTTP_POST, handleUpdate, this},
    };
    for (const httpd_uri_t& r : routes) httpd_register_uri_handler(server_, &r);
    httpd_register_err_handler(server_, HTTPD_404_NOT_FOUND, handleNotFound);
    return true;
}

bool WebUi::rebootDue() const {
    const uint32_t at = rebootAtMs_;
    return at != 0 && static_cast<int32_t>(millis() - at) >= 0;
}

void WebUi::scheduleReboot() { rebootAtMs_ = (millis() + REBOOT_DELAY_MS) | 1; }

bool WebUi::authorized(httpd_req_t* req) {
    if (cfg_.webPass.empty()) return true;

    char header[192];
    if (httpd_req_get_hdr_value_str(req, "Authorization", header, sizeof(header)) == ESP_OK &&
        strncmp(header, "Basic ", 6) == 0) {
        unsigned char decoded[160];
        size_t len = 0;
        if (mbedtls_base64_decode(decoded, sizeof(decoded) - 1, &len, reinterpret_cast<unsigned char*>(header + 6),
                                  strlen(header + 6)) == 0) {
            decoded[len] = '\0';
            const std::string expected = std::string(WEB_USER) + ":" + cfg_.webPass;
            if (expected == reinterpret_cast<char*>(decoded)) return true;
        }
    }
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"TCI to ICOM Tuner Interface\"");
    httpd_resp_sendstr(req, "Anmeldung erforderlich");
    return false;
}

esp_err_t WebUi::handleIndex(httpd_req_t* req) {
    std::lock_guard<std::recursive_mutex> lock(appMutex());
    if (!self(req)->authorized(req)) return ESP_OK;
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, INDEX_HTML_START, INDEX_HTML_END - INDEX_HTML_START);
}

esp_err_t WebUi::handleStatus(httpd_req_t* req) {
    std::lock_guard<std::recursive_mutex> lock(appMutex());
    WebUi& ui = *self(req);
    if (!ui.authorized(req)) return ESP_OK;
    const uint32_t now = millis();
    const bool sta = net::staConnected();

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "fw", esp_app_get_description()->version);
    cJSON_AddNumberToObject(root, "uptime", now / 1000);

    cJSON* wifi = cJSON_AddObjectToObject(root, "wifi");
    cJSON_AddBoolToObject(wifi, "connected", sta);
    cJSON_AddStringToObject(wifi, "ssid", net::staSsid().c_str());
    cJSON_AddStringToObject(wifi, "ip", net::staIp().c_str());
    cJSON_AddNumberToObject(wifi, "rssi", net::staRssi());
    cJSON_AddBoolToObject(wifi, "ap", net::apActive());
    cJSON_AddStringToObject(wifi, "ap_ssid", net::apSsid().c_str());

    cJSON* tci = cJSON_AddObjectToObject(root, "tci");
    cJSON_AddStringToObject(tci, "host", ui.cfg_.tciHost.c_str());
    cJSON_AddNumberToObject(tci, "port", ui.cfg_.tciPort);
    cJSON_AddBoolToObject(tci, "connected", ui.tci_.connected());
    cJSON_AddBoolToObject(tci, "ready", ui.tci_.ready());
    cJSON_AddStringToObject(tci, "device", ui.tci_.device().c_str());
    cJSON_AddStringToObject(tci, "protocol", ui.tci_.protocol().c_str());

    const Tuner& t = ui.tuner_;
    cJSON* tuner = cJSON_AddObjectToObject(root, "tuner");
    cJSON_AddStringToObject(tuner, "state", Tuner::stateId(t.state()));
    cJSON_AddBoolToObject(tuner, "key", t.keyActive());
    cJSON_AddNumberToObject(tuner, "trx", t.trx());
    cJSON_AddNumberToObject(tuner, "elapsed_ms", t.elapsedMs());
    addFloat(tuner, "swr", t.liveSwr(), 2);

    cJSON* history = cJSON_AddArrayToObject(root, "history");
    for (size_t i = 0; i < t.historyCount(); ++i) {
        const Tuner::Record& r = t.history(i);
        cJSON* e = cJSON_CreateObject();
        cJSON_AddNumberToObject(e, "ago_s", (now - r.finishedMs) / 1000);
        cJSON_AddNumberToObject(e, "trx", r.trx);
        cJSON_AddNumberToObject(e, "freq", r.freqHz);
        cJSON_AddStringToObject(e, "result", Tuner::resultId(r.result));
        addFloat(e, "swr", r.swr, 2);
        cJSON_AddNumberToObject(e, "duration_ms", r.durationMs);
        cJSON_AddItemToArray(history, e);
    }
    return sendJson(req, root);
}

esp_err_t WebUi::handleGetSettings(httpd_req_t* req) {
    std::lock_guard<std::recursive_mutex> lock(appMutex());
    WebUi& ui = *self(req);
    if (!ui.authorized(req)) return ESP_OK;
    const Settings& c = ui.cfg_;

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "wifi_ssid", c.wifiSsid.c_str());
    cJSON_AddBoolToObject(root, "wifi_pass_set", !c.wifiPass.empty());
    cJSON_AddStringToObject(root, "hostname", c.hostname.c_str());
    cJSON_AddBoolToObject(root, "web_pass_set", !c.webPass.empty());
    cJSON_AddStringToObject(root, "tci_host", c.tciHost.c_str());
    cJSON_AddNumberToObject(root, "tci_port", c.tciPort);
    cJSON_AddNumberToObject(root, "tune_trx", c.tuneTrx);
    cJSON_AddNumberToObject(root, "key_active_high", c.keyActiveHigh ? 1 : 0);
    cJSON_AddNumberToObject(root, "start_hold_ms", c.startHoldMs);
    cJSON_AddNumberToObject(root, "key_wait_ms", c.keyWaitMs);
    cJSON_AddNumberToObject(root, "tune_timeout_ms", c.tuneTimeoutMs);
    cJSON_AddNumberToObject(root, "swr_settle_ms", c.swrSettleMs);
    addFloat(root, "swr_max", c.swrMax, 1);
    return sendJson(req, root);
}

esp_err_t WebUi::handlePostSettings(httpd_req_t* req) {
    // Body zuerst ohne Sperre lesen: das kann bei langsamen Clients dauern
    if (req->content_len > MAX_BODY) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "zu gross");
    std::string body(req->content_len, '\0');
    for (size_t got = 0; got < body.size();) {
        const int n = httpd_req_recv(req, body.data() + got, body.size() - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (n <= 0) return ESP_FAIL;
        got += n;
    }

    std::lock_guard<std::recursive_mutex> lock(appMutex());
    WebUi& ui = *self(req);
    if (!ui.authorized(req)) return ESP_OK;

    Settings n = ui.cfg_;
    std::string v;
    if (formValue(body, "wifi_ssid", v)) n.wifiSsid = v, trim(n.wifiSsid);
    if (formValue(body, "hostname", v)) n.hostname = v, trim(n.hostname);
    if (formValue(body, "tci_host", v)) n.tciHost = v, trim(n.tciHost);
    // Passwortfelder: leer = unverändert
    if (formValue(body, "wifi_pass", v) && !v.empty()) n.wifiPass = v;
    if (formValue(body, "web_pass", v) && !v.empty()) n.webPass = v;
    if (formValue(body, "web_pass_clear", v)) n.webPass.clear();

    formNumber(body, "tci_port", n.tciPort);
    formNumber(body, "tune_trx", n.tuneTrx);
    uint8_t keyHigh = n.keyActiveHigh ? 1 : 0;
    formNumber(body, "key_active_high", keyHigh);
    n.keyActiveHigh = keyHigh != 0;
    formNumber(body, "start_hold_ms", n.startHoldMs);
    formNumber(body, "key_wait_ms", n.keyWaitMs);
    formNumber(body, "tune_timeout_ms", n.tuneTimeoutMs);
    formNumber(body, "swr_settle_ms", n.swrSettleMs);
    if (formValue(body, "swr_max", v) && !v.empty()) n.swrMax = strtof(v.c_str(), nullptr);
    n.sanitize();

    const bool reboot =
        n.wifiSsid != ui.cfg_.wifiSsid || n.wifiPass != ui.cfg_.wifiPass || n.hostname != ui.cfg_.hostname;
    const bool tciChanged = n.tciHost != ui.cfg_.tciHost || n.tciPort != ui.cfg_.tciPort;

    ui.cfg_ = n;
    ui.cfg_.save();
    if (tciChanged && !reboot) ui.tci_.configure(ui.cfg_.tciHost, ui.cfg_.tciPort);
    if (reboot) ui.scheduleReboot();

    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddBoolToObject(root, "reboot", reboot);
    return sendJson(req, root);
}

esp_err_t WebUi::handleReboot(httpd_req_t* req) {
    std::lock_guard<std::recursive_mutex> lock(appMutex());
    if (!self(req)->authorized(req)) return ESP_OK;
    self(req)->scheduleReboot();
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    return sendJson(req, root);
}

// Firmware-Update: Body ist die firmware.bin (application/octet-stream).
// Der Empfang läuft ohne appMutex, damit Hauptschleife und Tuner weiterarbeiten.
esp_err_t WebUi::handleUpdate(httpd_req_t* req) {
    WebUi& ui = *self(req);
    {
        std::lock_guard<std::recursive_mutex> lock(appMutex());
        if (!ui.authorized(req)) return ESP_OK;
        if (ui.tuner_.busy()) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "busy");
    }
    const char* error = receiveFirmware(req);
    if (error) {
        ESP_LOGE(TAG, "Update fehlgeschlagen: %s", error);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, error);
    }
    ESP_LOGI(TAG, "Update geschrieben, starte neu");
    ui.scheduleReboot();
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    return sendJson(req, root);
}

// Anfragen über den Access-Point an fremde Hosts werden auf die Konfigurationsseite
// umgeleitet, damit Smartphones und PCs das Captive Portal anzeigen.
esp_err_t WebUi::handleNotFound(httpd_req_t* req, httpd_err_code_t) {
    bool redirect = false;
    std::string apIp;
    {
        std::lock_guard<std::recursive_mutex> lock(appMutex());
        const uint32_t ip = net::apIp();
        if (net::apActive() && localAddress(req) == ip) {
            char host[64] = {};
            httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host));
            esp_ip4_addr_t addr = {ip};
            char buf[16];
            snprintf(buf, sizeof(buf), IPSTR, IP2STR(&addr));
            apIp = buf;
            redirect = apIp != host;
        }
    }
    if (redirect) {
        const std::string location = "http://" + apIp + "/";
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", location.c_str());
        return httpd_resp_send(req, nullptr, 0);
    }
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
    return ESP_FAIL;
}
