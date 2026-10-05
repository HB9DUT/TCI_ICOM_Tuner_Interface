#include "tci_client.h"

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <strings.h>

#include "app_util.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "network.h"

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
constexpr size_t MAX_OUTGOING = 32;
constexpr uint32_t LOOP_MS = 5;
constexpr UBaseType_t TASK_PRIORITY = 6;  // über dem HTTP-Server, unter dem Tuner
constexpr uint32_t TASK_STACK = 6144;
// TUNE:true so kurz nach READY stammt noch vom Verbindungsaufbau
constexpr uint32_t AT_CONNECT_WINDOW_MS = 1000;

char* trim(char* s) {
    while (isspace(static_cast<unsigned char>(*s))) ++s;
    char* end = s + strlen(s);
    while (end > s && isspace(static_cast<unsigned char>(end[-1]))) *--end = '\0';
    return s;
}

bool parseBool(const char* s) { return strcasecmp(s, "true") == 0; }

}  // namespace

void TciClient::configure(const std::string& host, uint16_t port) {
    std::lock_guard<std::mutex> lock(configMutex_);
    host_ = host;
    port_ = port;
    reconfigure_ = true;
}

void TciClient::startTask() { xTaskCreate(taskEntry, "tci", TASK_STACK, this, TASK_PRIORITY, nullptr); }

void TciClient::taskEntry(void* arg) {
    auto* self = static_cast<TciClient*>(arg);
    for (;;) {
        self->loop(net::staConnected());
        self->flushOutgoing();
        vTaskDelay(pdMS_TO_TICKS(LOOP_MS));
    }
}

void TciClient::loop(bool networkUp) {
    bool hasHost;
    bool reconfigure;
    {
        std::lock_guard<std::mutex> lock(configMutex_);
        hasHost = !host_.empty();
        reconfigure = reconfigure_;
        reconfigure_ = false;
    }
    if (reconfigure) {
        stop();
        if (!hasHost) ESP_LOGW(TAG, "kein Server konfiguriert");
    }
    if (networkUp && !client_ && hasHost && static_cast<int32_t>(millis() - reconnectAtMs_) >= 0) {
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

uint32_t TciClient::vfoHz(int trx) const { return (trx >= 0 && trx < MAX_TRX) ? vfo_[trx].load() : 0; }

bool TciClient::transmitting(int trx) const { return trx >= 0 && trx < MAX_TRX && trx_[trx].load(); }

std::string TciClient::device() const {
    std::lock_guard<std::mutex> lock(infoMutex_);
    return device_;
}

std::string TciClient::protocol() const {
    std::lock_guard<std::mutex> lock(infoMutex_);
    return protocol_;
}

void TciClient::setTune(int trx, bool on) { enqueue("tune:%d,%s;", trx, on ? "true" : "false"); }

void TciClient::queryTune(int trx) { enqueue("tune:%d;", trx); }

void TciClient::setTxSensors(bool on, uint16_t intervalMs) {
    if (on) {
        enqueue("tx_sensors_enable:true,%u;", intervalMs);
    } else {
        enqueue("tx_sensors_enable:false;");
    }
}

void TciClient::start() {
    std::string uri;
    {
        std::lock_guard<std::mutex> lock(configMutex_);
        uri = "ws://" + host_ + ":" + std::to_string(port_) + "/";
    }
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
    reconnectAtMs_ = millis();  // sofort neu verbinden
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
    if (e.type == Event::Type::Data && self->queue_.size() >= MAX_QUEUED_EVENTS) {
        // Daten verwerfen, aber die Lücke markieren: sonst setzt der Parser die
        // Bruchstücke davor und danach zu einem falschen Befehl zusammen.
        if (self->queue_.back().type == Event::Type::Lost) return;
        e.type = Event::Type::Lost;
        e.data.clear();
    }
    self->queue_.push_back(std::move(e));
}

void TciClient::handleEvent(Event& e) {
    switch (e.type) {
        case Event::Type::Connected:
            connected_ = true;
            rx_.clear();
            resync_ = false;
            ESP_LOGI(TAG, "verbunden, warte auf READY");
            break;

        case Event::Type::Disconnected:
            // Der Client-Task kann sich hier beenden (z.B. sauberer Close durch den
            // Server), ohne selbst erneut zu verbinden. Client verwerfen, damit loop()
            // nach RECONNECT_INTERVAL_MS einen neuen anlegt.
            handleDisconnect();
            if (client_) {
                esp_websocket_client_destroy(client_);
                client_ = nullptr;
            }
            reconnectAtMs_ = millis() + RECONNECT_INTERVAL_MS;
            break;

        case Event::Type::Lost:
            ESP_LOGW(TAG, "Empfangsdaten verworfen, warte auf den nächsten Befehl");
            rx_.clear();
            resync_ = true;
            break;

        case Event::Type::Data: {
            if (resync_) {
                // Rest des angeschnittenen Befehls überspringen
                const size_t end = e.data.find(';');
                if (end == std::string::npos) break;
                e.data.erase(0, end + 1);
                resync_ = false;
            }
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
    resync_ = false;
    {
        std::lock_guard<std::mutex> lock(infoMutex_);
        device_.clear();
        protocol_.clear();
    }
    for (auto& t : trx_) t = false;
    // Nichts Veraltetes nach dem Wiederverbinden senden (z.B. ein TUNE:true)
    {
        std::lock_guard<std::mutex> lock(outMutex_);
        out_.clear();
    }
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
        const int trx = atoi(argv[0]);
        const bool atConnect = !ready_ || millis() - readyAtMs_ < AT_CONNECT_WINDOW_MS;
        // TRX jetzt lesen, nicht erst im Tuner-Task: deskHPSDR schickt TRX:true
        // direkt hinter TUNE:true, Thetis TRX:false vor einem echten TUNE:false
        if (onTune) onTune(trx, parseBool(argv[1]), vfoHz(trx), atConnect, transmitting(trx));
    } else if (strcmp(name, "tx_sensors") == 0 && argc >= 5) {
        // tx_sensors:trx,mic_dbm,rms_w,peak_w,swr;
        if (onTxSensors) onTxSensors(atoi(argv[0]), strtof(argv[4], nullptr));
    } else if (strcmp(name, "vfo") == 0 && argc >= 3) {
        const int trx = atoi(argv[0]);
        if (trx >= 0 && trx < MAX_TRX && atoi(argv[1]) == 0) vfo_[trx] = strtoul(argv[2], nullptr, 10);
    } else if (strcmp(name, "trx") == 0 && argc >= 2) {
        const int trx = atoi(argv[0]);
        if (trx >= 0 && trx < MAX_TRX) {
            trx_[trx] = parseBool(argv[1]);
            if (onTrx) onTrx(trx, trx_[trx]);
        }
    } else if (strcmp(name, "ready") == 0) {
        ESP_LOGI(TAG, "bereit (%s, %s)", device().c_str(), protocol().c_str());
        readyAtMs_ = millis();
        setReady(true);
    } else if (strcmp(name, "device") == 0 && argc >= 1) {
        std::lock_guard<std::mutex> lock(infoMutex_);
        device_ = argv[0];
    } else if (strcmp(name, "protocol") == 0 && argc >= 2) {
        std::lock_guard<std::mutex> lock(infoMutex_);
        protocol_ = std::string(argv[0]) + " " + argv[1];
    }
}

void TciClient::enqueue(const char* fmt, ...) {
    if (!connected_) return;
    char buf[64];
    va_list ap;
    va_start(ap, fmt);
    const int len = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (len <= 0 || static_cast<size_t>(len) >= sizeof(buf)) return;
    std::lock_guard<std::mutex> lock(outMutex_);
    if (out_.size() >= MAX_OUTGOING) {
        ESP_LOGW(TAG, "Sende-Queue voll, verworfen: %s", buf);
        return;
    }
    out_.emplace_back(buf, len);
}

// Läuft im TCI-Task; nur hier wird gesendet (blockiert höchstens SEND_TIMEOUT je Befehl).
void TciClient::flushOutgoing() {
    for (;;) {
        std::string cmd;
        {
            std::lock_guard<std::mutex> lock(outMutex_);
            if (out_.empty()) return;
            if (!connected_ || !client_) {
                out_.clear();
                return;
            }
            cmd = std::move(out_.front());
            out_.pop_front();
        }
        if (esp_websocket_client_send_text(client_, cmd.data(), cmd.size(), SEND_TIMEOUT) < 0) {
            ESP_LOGW(TAG, "Senden fehlgeschlagen: %s", cmd.c_str());
        }
    }
}

void TciClient::setReady(bool ready) {
    if (ready == ready_) return;
    ready_ = ready;
    if (onReady) onReady(ready);
}
