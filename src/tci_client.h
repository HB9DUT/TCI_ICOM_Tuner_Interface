#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>

#include "esp_event.h"
#include "esp_websocket_client.h"

// Schlanker TCI-Client (ExpertSDR3, https://github.com/ExpertSDR3/TCI).
// Wertet nur die Befehle aus, die für die Tuner-Steuerung gebraucht werden.
//
// Der WebSocket-Client läuft in einem eigenen Task; dessen Ereignisse werden
// in eine Queue gestellt und in loop() (Hauptschleife) verarbeitet.
class TciClient {
public:
    static constexpr int MAX_TRX = 4;

    std::function<void(bool ready)> onReady;
    std::function<void(int trx, bool on)> onTune;
    std::function<void(int trx, float swr)> onTxSensors;

    // Server festlegen; wirkt beim nächsten loop(). Leerer Host = keine Verbindung.
    void configure(const std::string& host, uint16_t port);
    void loop(bool networkUp);

    bool connected() const { return connected_; }
    bool ready() const { return ready_; }
    const std::string& device() const { return device_; }
    const std::string& protocol() const { return protocol_; }
    uint32_t vfoHz(int trx) const;  // Frequenz VFO A

    void setTune(int trx, bool on);
    void queryTune(int trx);
    void setTxSensors(bool on, uint16_t intervalMs = 100);

private:
    struct Event {
        enum class Type : uint8_t { Connected, Disconnected, Data } type;
        std::string data;
    };

    static void wsEventHandler(void* arg, esp_event_base_t base, int32_t id, void* eventData);
    void start();
    void stop();
    void handleEvent(Event& e);
    void handleDisconnect();
    void handleCommand(const char* cmd, size_t len);
    void send(const char* fmt, ...) __attribute__((format(printf, 2, 3)));
    void setReady(bool ready);

    esp_websocket_client_handle_t client_ = nullptr;
    std::string host_;
    uint16_t port_ = 0;
    bool reconfigure_ = false;
    uint32_t reconnectAtMs_ = 0;  // 0 = sofort verbinden

    std::mutex queueMutex_;
    std::deque<Event> queue_;
    std::string rx_;  // empfangene Daten ohne abschliessendes ';'

    bool connected_ = false;
    bool ready_ = false;
    std::string device_;
    std::string protocol_;
    uint32_t vfo_[MAX_TRX] = {};
};
