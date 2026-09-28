#include "tci_client.h"

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <strings.h>

#include "esp_log.h"

namespace {

constexpr const char* TAG = "tci";

constexpr int RECONNECT_INTERVAL_MS = 5000;
constexpr int NETWORK_TIMEOUT_MS = 5000;
constexpr int PING_INTERVAL_S = 10;
constexpr int PONG_TIMEOUT_S = 25;
constexpr TickType_t SEND_TIMEOUT = pdMS_TO_TICKS(500);
constexpr size_t MAX_QUEUED_EVENTS = 64;
constexpr size_t MAX_RX_BUFFER = 4096;
constexpr int MAX_ARGS = 8;

char* trim(char* s) {
    while (isspace(static_cast<unsigned char>(*s))) ++s;
    char* end = s + strlen(s);
    while (end > s && isspace(static_cast<unsigned char>(end[-1]))) *--end = '\0';
    return s;
}

bool parseBool(const char* s) { return strcasecmp(s, "true") == 0; }

}  // namespace

void TciClient::configure(const std::string& host, uint16_t port) {
    host_ = host;
    port_ = port;
    reconfigure_ = true;
}

void TciClient::loop(bool networkUp) {
    if (reconfigure_) {
        reconfigure_ = false;
        stop();
        if (host_.empty()) ESP_LOGW(TAG, "kein Server konfiguriert");
    }
    if (networkUp && !client_ && !host_.empty()) {
        start();
    } else if (!networkUp && client_) {
        stop();
    }

    std::deque<Event> events;
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        events.swap(queue_);
    }
    for (Event& e : events) handleEvent(e);
}

uint32_t TciClient::vfoHz(int trx) const { return (trx >= 0 && trx < MAX_TRX) ? vfo_[trx] : 0; }

void TciClient::setTune(int trx, bool on) { send("tune:%d,%s;", trx, on ? "true" : "false"); }

void TciClient::queryTune(int trx) { send("tune:%d;", trx); }

void TciClient::setTxSensors(bool on, uint16_t intervalMs) {
    if (on) {
        send("tx_sensors_enable:true,%u;", intervalMs);
    } else {
        send("tx_sensors_enable:false;");
    }
}

void TciClient::start() {
    const std::string uri = "ws://" + host_ + ":" + std::to_string(port_) + "/";
    ESP_LOGI(TAG, "Server %s", uri.c_str());

    esp_websocket_client_config_t cfg = {};
    cfg.uri = uri.c_str();  // wird von esp_websocket_client_init kopiert
    cfg.reconnect_timeout_ms = RECONNECT_INTERVAL_MS;
    cfg.network_timeout_ms = NETWORK_TIMEOUT_MS;
    cfg.ping_interval_sec = PING_INTERVAL_S;
    cfg.pingpong_timeout_sec = PONG_TIMEOUT_S;

    client_ = esp_websocket_client_init(&cfg);
    if (!client_) {
        ESP_LOGE(TAG, "WebSocket-Client konnte nicht angelegt werden");
        return;
    }
    esp_websocket_register_events(client_, WEBSOCKET_EVENT_ANY, wsEventHandler, this);
    if (esp_websocket_client_start(client_) != ESP_OK) {
        ESP_LOGE(TAG, "WebSocket-Client konnte nicht gestartet werden");
        esp_websocket_client_destroy(client_);
        client_ = nullptr;
    }
}

void TciClient::stop() {
    if (client_) {
        esp_websocket_client_destroy(client_);  // stoppt auch den Client-Task
        client_ = nullptr;
    }
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        queue_.clear();
    }
    handleDisconnect();
}

// Läuft im Task des WebSocket-Clients: nur Daten kopieren und einreihen.
void TciClient::wsEventHandler(void* arg, esp_event_base_t, int32_t id, void* eventData) {
    auto* self = static_cast<TciClient*>(arg);
    const auto* d = static_cast<const esp_websocket_event_data_t*>(eventData);

    Event e;
    switch (id) {
        case WEBSOCKET_EVENT_CONNECTED:
            e.type = Event::Type::Connected;
            break;
        case WEBSOCKET_EVENT_DISCONNECTED:
        case WEBSOCKET_EVENT_CLOSED:
            e.type = Event::Type::Disconnected;
            break;
        case WEBSOCKET_EVENT_DATA:
            // Nur Textframes (0x1) und deren Fortsetzungen (0x0); Binärdaten und Ping/Pong ignorieren
            if ((d->op_code != 0x1 && d->op_code != 0x0) || d->data_len <= 0) return;
            e.type = Event::Type::Data;
            e.data.assign(d->data_ptr, d->data_len);
            break;
        default:
            return;
    }

    std::lock_guard<std::mutex> lock(self->queueMutex_);
    if (self->queue_.size() < MAX_QUEUED_EVENTS || e.type != Event::Type::Data) self->queue_.push_back(std::move(e));
}

void TciClient::handleEvent(Event& e) {
    switch (e.type) {
        case Event::Type::Connected:
            connected_ = true;
            rx_.clear();
            ESP_LOGI(TAG, "verbunden, warte auf READY");
            break;

        case Event::Type::Disconnected:
            handleDisconnect();
            break;

        case Event::Type::Data: {
            // Befehle enden mit ';' und können über Frames verteilt sein.
            rx_ += e.data;
            size_t start = 0;
            for (size_t pos; (pos = rx_.find(';', start)) != std::string::npos; start = pos + 1) {
                handleCommand(rx_.data() + start, pos - start);
            }
            rx_.erase(0, start);
            if (rx_.size() > MAX_RX_BUFFER) rx_.clear();
            break;
        }
    }
}

void TciClient::handleDisconnect() {
    if (connected_) ESP_LOGW(TAG, "Verbindung getrennt");
    connected_ = false;
    rx_.clear();
    device_.clear();
    protocol_.clear();
    setReady(false);
}

void TciClient::handleCommand(const char* cmd, size_t len) {
    // Lange Befehle (z.B. modulations_list) werden gekürzt; sie werden nicht ausgewertet.
    char buf[160];
    len = std::min(len, sizeof(buf) - 1);
    memcpy(buf, cmd, len);
    buf[len] = '\0';

    char* name = trim(buf);
    if (*name == '\0') return;

    char* args = strchr(name, ':');
    if (args) *args++ = '\0';
    for (char* p = name; *p; ++p) *p = static_cast<char>(tolower(static_cast<unsigned char>(*p)));

    char* argv[MAX_ARGS];
    int argc = 0;
    if (args) {
        char* save = nullptr;
        for (char* tok = strtok_r(args, ",", &save); tok && argc < MAX_ARGS; tok = strtok_r(nullptr, ",", &save)) {
            argv[argc++] = trim(tok);
        }
    }

    if (strcmp(name, "tune") == 0 && argc >= 2) {
        if (onTune) onTune(atoi(argv[0]), parseBool(argv[1]));
    } else if (strcmp(name, "tx_sensors") == 0 && argc >= 5) {
        // tx_sensors:trx,mic_dbm,rms_w,peak_w,swr;
        if (onTxSensors) onTxSensors(atoi(argv[0]), strtof(argv[4], nullptr));
    } else if (strcmp(name, "vfo") == 0 && argc >= 3) {
        const int trx = atoi(argv[0]);
        if (trx >= 0 && trx < MAX_TRX && atoi(argv[1]) == 0) vfo_[trx] = strtoul(argv[2], nullptr, 10);
    } else if (strcmp(name, "ready") == 0) {
        ESP_LOGI(TAG, "bereit (%s, %s)", device_.c_str(), protocol_.c_str());
        setReady(true);
    } else if (strcmp(name, "device") == 0 && argc >= 1) {
        device_ = argv[0];
    } else if (strcmp(name, "protocol") == 0 && argc >= 2) {
        protocol_ = std::string(argv[0]) + " " + argv[1];
    }
}

void TciClient::send(const char* fmt, ...) {
    if (!connected_ || !client_) return;
    char buf[64];
    va_list ap;
    va_start(ap, fmt);
    const int len = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (len <= 0 || static_cast<size_t>(len) >= sizeof(buf)) return;
    if (esp_websocket_client_send_text(client_, buf, len, SEND_TIMEOUT) < 0) ESP_LOGW(TAG, "Senden fehlgeschlagen: %s", buf);
}

void TciClient::setReady(bool ready) {
    if (ready == ready_) return;
    ready_ = ready;
    if (onReady) onReady(ready);
}
