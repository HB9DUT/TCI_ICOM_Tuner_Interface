#pragma once

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <mutex>

#include "app_util.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "settings.h"
#include "tci_client.h"

// Emuliert die Funkgeräteseite einer ICOM-AH-4-Schnittstelle:
//
//   Das SDR-Programm meldet TUNE:trx,true (Tune-Träger ist an)
//   -> START aktiv, bis der Tuner KEY aktiviert, dann noch startHoldMs lang
//   -> Tuner stimmt ab, solange KEY aktiv ist
//   -> Tuner gibt KEY frei: fertig. Setzt er KEY kurz danach (20 ms Lücke)
//      nochmals, meldet er einen Fehlschlag.
//   -> optional SWR-Messung, dann TUNE:trx,false an das SDR-Programm
//
// Jede Sitzung endet spätestens nach dem Tune-Timeout mit TUNE:trx,false,
// damit der Träger nie unbegrenzt ansteht.
//
// Die Zustandsmaschine läuft in einem eigenen Task mit hoher Priorität und gehört
// ihm allein. Andere Tasks (TCI, Weboberfläche) schicken Befehle über eine Queue
// und lesen den Zustand über snapshot(). KEY wird per Interrupt mit Zeitstempel
// erfasst, damit auch kurze Pulse nicht verloren gehen.
class Tuner {
public:
    enum class State : uint8_t { Idle, WaitKey, Tuning, Settle, Stopping };
    enum class Result : uint8_t { None, Ok, SwrHigh, TunerFailed, NoResponse, Timeout, Aborted, StopFailed };

    // Sperrgründe: solange einer gesetzt ist, beginnt keine neue Sitzung
    enum Lock : uint8_t { LOCK_UPDATE = 1, LOCK_REBOOT = 2, LOCK_SCAN = 4 };

    struct Config {
        int8_t tuneTrx = -1;
        bool keyActiveHigh = true;
        uint16_t startHoldMs = 250;
        uint16_t keyWaitMs = 2000;
        uint16_t tuneTimeoutMs = 20000;
        uint16_t swrSettleMs = 300;
        float swrMax = 2.0f;

        static Config from(const Settings& s);
    };

    struct Record {
        uint32_t finishedMs;
        uint32_t durationMs;
        uint32_t freqHz;
        float swr;  // NaN = nicht gemessen
        int8_t trx;
        Result result;
    };
    static constexpr size_t HISTORY_SIZE = 10;

    // Zustand für Weboberfläche und LED, vom Tuner-Task nach jedem Durchlauf abgelegt
    struct Snapshot {
        State state = State::Idle;
        bool key = false;
        int trx = -1;
        uint32_t elapsedMs = 0;
        float liveSwr = NAN;
        bool lastFailed = false;  // Abbruch durch den Benutzer zählt nicht
        uint32_t lastFinishedMs = 0;
        size_t historyCount = 0;
        Record history[HISTORY_SIZE] = {};  // 0 = neuester Eintrag
    };

    explicit Tuner(TciClient& tci) : tci_(tci) {}

    // START-Leitung sofort in den inaktiven Zustand (vor allem anderen aufrufen)
    void initOutput();
    // KEY-Interrupt einrichten und Task starten
    void start(const Config& cfg);

    // Aus beliebigen Tasks aufrufbar
    void applyConfig(const Config& cfg);
    void onTuneEvent(int trx, bool on, uint32_t freqHz, bool atConnect, bool carrierOn);
    void onTxSensors(int trx, float swr);
    void onTciReady(bool ready);

    // Tune über die Weboberfläche. Das Modul setzt TUNE dann selbst und ist bei
    // SDR-Programmen mit Sendeberechtigung (z.B. deskHPSDR) Besitzer des Sendens,
    // darf den Tune also auch wieder beenden.
    void requestStart();
    void requestStop();

    void setLock(Lock reason, bool on);
    // Sperrt nur, wenn gerade keine Abstimmung läuft (sonst false, nichts gesperrt)
    bool lockIfIdle(Lock reason);
    bool locked() const { return locks_.load() != 0; }
    bool busy() const { return busy_.load(); }
    Snapshot snapshot() const;

    static const char* stateId(State s);
    static const char* resultId(Result r);

private:
    struct Command {
        enum class Type : uint8_t { Config, Tune, TxSensors, TciReady, WebStart, WebStop } type;
        int trx;
        bool on;
        bool atConnect;
        bool carrierOn;
        uint32_t freqHz;
        float swr;
        Config cfg;
    };

    static void taskEntry(void* arg);
    void run();
    void post(const Command& cmd);
    void handle(const Command& cmd, uint32_t now);
    void processKey(uint32_t now);
    void settleKey(uint32_t t);
    void onKeyChange(bool active, uint32_t t);
    void update(uint32_t now);
    void publish(uint32_t now);
    bool claimStart(int trx, const char* source);
    void startSession(int trx, uint32_t freqHz, uint32_t now, bool start = true);
    void requestStop(Result result, uint32_t now);
    void sendStop(uint32_t now);
    void finish(Result result, uint32_t now);
    Result evaluateSwr() const;
    void setStart(bool active);
    void setState(State s);

    TciClient& tci_;
    QueueHandle_t commands_ = nullptr;
    std::atomic<uint8_t> locks_{0};
    std::atomic<bool> busy_{false};

    // Ab hier nur im Tuner-Task
    Config cfg_;
    State state_ = State::Idle;
    Result pendingResult_ = Result::None;
    int trx_ = -1;
    uint32_t freqHz_ = 0;
    uint32_t startMs_ = 0;
    uint32_t keyActiveMs_ = 0;
    bool startActive_ = false;
    uint32_t tuneEndMs_ = 0;
    uint32_t stopSentMs_ = 0;
    uint8_t stopAttempts_ = 0;
    // Träger war vor START schon an (z.B. Thetis): erst aus, dann START und Träger wieder ein
    bool waitCarrierOff_ = false;
    bool tuneOffSeen_ = false;
    uint32_t ignoreTuneOffUntilMs_ = 0;  // verspätete TUNE:false-Echos bis dahin ignorieren
    float liveSwr_ = NAN;
    float settleSwr_ = NAN;

    bool keyRaw_ = false;
    bool keyStable_ = false;
    uint32_t keyChangedMs_ = 0;

    Record history_[HISTORY_SIZE] = {};
    size_t historyHead_ = 0;
    size_t historyCount_ = 0;
    Result lastResult_ = Result::None;
    uint32_t lastFinishedMs_ = 0;

    mutable std::mutex snapshotMutex_;
    Snapshot snapshot_;
};
