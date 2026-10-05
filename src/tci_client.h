#pragma once

#include <atomic>
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
// Läuft in einem eigenen Task: Verbindungsaufbau, Auswertung der empfangenen
// Daten und Senden. Befehle anderer Tasks (setTune usw.) landen in einer
// Sende-Queue, damit ein hängendes Netzwerk nur diesen Task aufhält.
// Die Callbacks werden im TCI-Task aufgerufen.
class TciClient {
public:
    static constexpr int MAX_TRX = 4;

    std::function<void(bool ready)> onReady;
    // atConnect: TUNE wurde beim Verbindungsaufbau gemeldet, nicht durch eine neue Anforderung
    // carrierOn: beim Eintreffen dieses TUNE war laut TRX-Meldung gesendet worden
    std::function<void(int trx, bool on, uint32_t freqHz, bool atConnect, bool carrierOn)> onTune;
    std::function<void(int trx, float swr)> onTxSensors;
    // TRX-Meldung (Sendezustand); transmitting() liefert denselben Wert
    std::function<void(int trx, bool on)> onTrx;

    // Server festlegen; wirkt im nächsten Durchlauf. Leerer Host = keine Verbindung.
    void configure(const std::string& host, uint16_t port);
    void startTask();

    // Aus beliebigen Tasks aufrufbar
    bool connected() const { return connected_.load(); }
    bool ready() const { return ready_.load(); }
    std::string device() const;
    std::string protocol() const;
    uint32_t vfoHz(int trx) const;      // Frequenz VFO A
    bool transmitting(int trx) const;  // laut TRX-Meldung des Servers

    void setTune(int trx, bool on);
    void queryTune(int trx);
    void setTxSensors(bool on, uint16_t intervalMs = 100);

private:
    struct Event {
        enum class Type : uint8_t { Connected, Disconnected, Data, Lost } type;
        std::string data;
    };

    static void taskEntry(void* arg);
    static void wsEventHandler(void* arg, esp_event_base_t base, int32_t id, void* eventData);
    void loop(bool networkUp);
    void start();
    void stop();
    void handleEvent(Event& e);
    void handleDisconnect();
    void handleCommand(const char* cmd, size_t len);
    void enqueue(const char* fmt, ...) __attribute__((format(printf, 2, 3)));
    void flushOutgoing();
    void setReady(bool ready);

    esp_websocket_client_handle_t client_ = nullptr;
    uint32_t reconnectAtMs_ = 0;  // nächster Verbindungsversuch frühestens dann
    uint32_t readyAtMs_ = 0;
    std::string rx_;       // empfangene Daten ohne abschliessendes ';'
    bool resync_ = false;  // nach verworfenen Daten bis zum nächsten ';' überspringen

    std::mutex configMutex_;
    std::string host_;
    uint16_t port_ = 0;
    bool reconfigure_ = false;

    std::mutex queueMutex_;
    std::deque<Event> queue_;

    std::mutex outMutex_;
    std::deque<std::string> out_;

    std::atomic<bool> connected_{false};
    std::atomic<bool> ready_{false};
    mutable std::mutex infoMutex_;
    std::string device_;
    std::string protocol_;
    std::atomic<uint32_t> vfo_[MAX_TRX] = {};
    std::atomic<bool> trx_[MAX_TRX] = {};
};
