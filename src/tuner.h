#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>

#include "app_util.h"
#include "settings.h"
#include "tci_client.h"

// Emuliert die Funkgeräteseite einer ICOM-AH-4-Schnittstelle:
//
//   ExpertSDR3 meldet TUNE:trx,true (Tune-Träger ist an)
//   -> START aktiv, bis der Tuner KEY aktiviert, dann noch startHoldMs lang
//   -> Tuner stimmt ab, solange KEY aktiv ist
//   -> Tuner gibt KEY frei: fertig. Setzt er KEY kurz danach (20 ms Lücke)
//      nochmals, meldet er einen Fehlschlag.
//   -> optional SWR-Messung, dann TUNE:trx,false an ExpertSDR3
//
// Jede Sitzung endet spätestens nach dem Tune-Timeout mit TUNE:trx,false,
// damit der Träger nie unbegrenzt ansteht.
class Tuner {
public:
    enum class State : uint8_t { Idle, WaitKey, Tuning, Settle, Stopping };
    enum class Result : uint8_t { None, Ok, SwrHigh, TunerFailed, NoResponse, Timeout, Aborted, StopFailed };

    struct Record {
        uint32_t finishedMs;
        uint32_t durationMs;
        uint32_t freqHz;
        float swr;  // NaN = nicht gemessen
        int8_t trx;
        Result result;
    };
    static constexpr size_t HISTORY_SIZE = 10;

    Tuner(const Settings& cfg, TciClient& tci) : cfg_(cfg), tci_(tci) {}

    void begin();
    void update();

    void onTuneEvent(int trx, bool on);
    void onTxSensors(int trx, float swr);
    void onTciReady(bool ready);

    // Tune über die Weboberfläche. Das Modul setzt TUNE dann selbst und ist bei
    // SDR-Programmen mit Sendeberechtigung (z.B. deskHPSDR) Besitzer des Sendens,
    // darf den Tune also auch wieder beenden.
    bool startTune();
    bool stopTune();

    State state() const { return state_; }
    bool busy() const { return state_ != State::Idle; }
    bool keyActive() const { return keyStable_; }
    int trx() const { return trx_; }
    uint32_t elapsedMs() const { return busy() ? millis() - startMs_ : 0; }
    float liveSwr() const { return busy() ? liveSwr_ : NAN; }

    // Letzte Sitzung mit Fehler beendet (Abbruch durch den Benutzer zählt nicht)
    bool lastFailed() const;
    uint32_t lastFinishedMs() const { return lastFinishedMs_; }

    size_t historyCount() const { return historyCount_; }
    const Record& history(size_t i) const;  // 0 = neuester Eintrag

    static const char* stateId(State s);
    static const char* resultId(Result r);

private:
    void startSession(int trx, uint32_t now);
    void requestStop(Result result, uint32_t now);
    void sendStop(uint32_t now);
    void finish(Result result);
    Result evaluateSwr() const;
    void setStart(bool active);
    void readKey(uint32_t now);
    bool keyLevelActive() const;

    const Settings& cfg_;
    TciClient& tci_;

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
    float liveSwr_ = NAN;
    float settleSwr_ = NAN;

    bool keyRaw_ = false;
    bool keyStable_ = false;
    bool keyUnexpectedLogged_ = false;
    uint32_t keyChangedMs_ = 0;

    Record history_[HISTORY_SIZE] = {};
    size_t historyHead_ = 0;
    size_t historyCount_ = 0;
    Result lastResult_ = Result::None;
    uint32_t lastFinishedMs_ = 0;
};
