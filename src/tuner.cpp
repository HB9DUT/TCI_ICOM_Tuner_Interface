#include "tuner.h"

#include <algorithm>
#include <cinttypes>

#include "esp_log.h"
#include "hw_config.h"

namespace {

constexpr const char* TAG = "tuner";
// Kurz genug, um die 20-ms-Lücke der Fehlschlag-Meldung zu erkennen (Hauptschleife: 5 ms)
constexpr uint32_t KEY_DEBOUNCE_MS = 5;
// Wird KEY innerhalb dieser Zeit nach der Freigabe erneut aktiv, meldet der Tuner einen Fehlschlag
constexpr uint32_t FAIL_PULSE_WINDOW_MS = 100;
constexpr uint16_t TX_SENSORS_INTERVAL_MS = 100;
constexpr uint32_t STOP_RETRY_MS = 500;
constexpr uint8_t STOP_MAX_ATTEMPTS = 6;

}  // namespace

void Tuner::begin() {
    setStart(false);  // Pegel setzen, bevor der Pin Ausgang wird
    gpio_config_t out = {};
    out.pin_bit_mask = 1ULL << hw::PIN_ATU_START;
    out.mode = GPIO_MODE_OUTPUT;
    gpio_config(&out);

    gpio_config_t in = {};
    in.pin_bit_mask = 1ULL << hw::PIN_ATU_KEY;
    in.mode = GPIO_MODE_INPUT;
    in.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&in);

    keyRaw_ = keyStable_ = keyLevelActive();
}

bool Tuner::keyLevelActive() const { return gpio_get_level(hw::PIN_ATU_KEY) == (cfg_.keyActiveHigh ? 1 : 0); }

void Tuner::update() {
    const uint32_t now = millis();
    readKey(now);
    const uint32_t elapsed = now - startMs_;

    switch (state_) {
        case State::Idle:
            if (keyStable_ && !keyUnexpectedLogged_) ESP_LOGW(TAG, "KEY aktiv ohne Tune-Anforderung");
            keyUnexpectedLogged_ = keyStable_;
            break;

        case State::WaitKey:
            if (keyStable_) {
                ESP_LOGI(TAG, "stimmt ab (KEY nach %" PRIu32 " ms)", elapsed);
                keyActiveMs_ = now;
                state_ = State::Tuning;
            } else if (elapsed >= cfg_.keyWaitMs) {
                ESP_LOGW(TAG, "keine Antwort auf START");
                requestStop(Result::NoResponse, now);
            }
            break;

        case State::Tuning:
            if (startActive_ && now - keyActiveMs_ >= cfg_.startHoldMs) setStart(false);
            if (!keyStable_) {
                ESP_LOGI(TAG, "KEY frei nach %" PRIu32 " ms", elapsed);
                setStart(false);
                tuneEndMs_ = now;
                settleSwr_ = NAN;
                state_ = State::Settle;
            } else if (elapsed >= cfg_.tuneTimeoutMs) {
                ESP_LOGW(TAG, "Timeout nach %" PRIu32 " ms", elapsed);
                requestStop(Result::Timeout, now);
            }
            break;

        case State::Settle: {
            // Erst nach dem Fehlschlag-Fenster ist sicher, dass der Tuner erfolgreich war
            const uint32_t sinceEnd = now - tuneEndMs_;
            if (keyStable_ && sinceEnd <= FAIL_PULSE_WINDOW_MS) {
                ESP_LOGW(TAG, "Tuner meldet Fehlschlag");
                requestStop(Result::TunerFailed, now);
            } else if (sinceEnd >= std::max<uint32_t>(cfg_.swrSettleMs, FAIL_PULSE_WINDOW_MS)) {
                requestStop(cfg_.swrSettleMs > 0 ? evaluateSwr() : Result::Ok, now);
            }
            break;
        }

        case State::Stopping:
            if (now - stopSentMs_ >= STOP_RETRY_MS) {
                if (stopAttempts_ >= STOP_MAX_ATTEMPTS) {
                    finish(Result::StopFailed);
                } else {
                    sendStop(now);
                }
            }
            break;
    }
}

void Tuner::onTuneEvent(int trx, bool on) {
    if (on) {
        // Jeder gemeldete Tune-Träger bekommt eine Sitzung mit Timeout.
        // Weitere TUNE:true während einer laufenden Sitzung sind Echos/Antworten.
        if (state_ == State::Idle && (cfg_.tuneTrx < 0 || trx == cfg_.tuneTrx)) startSession(trx, millis());
        return;
    }
    if (state_ == State::Idle || trx != trx_) return;
    if (state_ == State::Stopping) {
        finish(pendingResult_);
    } else {
        ESP_LOGI(TAG, "Tune in ExpertSDR3 beendet");
        finish(Result::Aborted);
    }
}

void Tuner::onTxSensors(int trx, float swr) {
    if (state_ == State::Idle || trx != trx_) return;
    liveSwr_ = swr;
    if (state_ == State::Settle) settleSwr_ = swr;
}

void Tuner::onTciReady(bool ready) {
    if (!ready && busy()) {
        ESP_LOGW(TAG, "TCI-Verbindung verloren");
        finish(Result::Aborted);
    }
}

bool Tuner::lastFailed() const {
    return lastResult_ != Result::None && lastResult_ != Result::Ok && lastResult_ != Result::Aborted;
}

const Tuner::Record& Tuner::history(size_t i) const {
    return history_[(historyHead_ + HISTORY_SIZE - 1 - i) % HISTORY_SIZE];
}

void Tuner::startSession(int trx, uint32_t now) {
    trx_ = trx;
    freqHz_ = tci_.vfoHz(trx);
    startMs_ = now;
    liveSwr_ = NAN;
    settleSwr_ = NAN;
    pendingResult_ = Result::None;
    ESP_LOGI(TAG, "Tune-Anforderung TRX %d, %.3f MHz", trx, freqHz_ / 1e6);
    tci_.setTxSensors(true, TX_SENSORS_INTERVAL_MS);
    setStart(true);
    state_ = State::WaitKey;
}

void Tuner::requestStop(Result result, uint32_t now) {
    setStart(false);
    pendingResult_ = result;
    stopAttempts_ = 0;
    state_ = State::Stopping;
    sendStop(now);
}

// Setzt TUNE aus und fragt den Zustand ab. ExpertSDR3 sperrt einen Parameter
// bis 200 ms nach einer Änderung; deshalb wird bis zur Bestätigung wiederholt.
void Tuner::sendStop(uint32_t now) {
    tci_.setTune(trx_, false);
    tci_.queryTune(trx_);
    stopSentMs_ = now;
    ++stopAttempts_;
}

void Tuner::finish(Result result) {
    setStart(false);
    tci_.setTxSensors(false);

    const uint32_t now = millis();
    const float swr = settleSwr_;
    history_[historyHead_] = {now, now - startMs_, freqHz_, swr, static_cast<int8_t>(trx_), result};
    historyHead_ = (historyHead_ + 1) % HISTORY_SIZE;
    if (historyCount_ < HISTORY_SIZE) ++historyCount_;

    if (std::isnan(swr)) {
        ESP_LOGI(TAG, "Ergebnis %s", resultId(result));
    } else {
        ESP_LOGI(TAG, "Ergebnis %s, SWR %.2f", resultId(result), swr);
    }

    lastResult_ = result;
    lastFinishedMs_ = now;
    state_ = State::Idle;
    trx_ = -1;
}

Tuner::Result Tuner::evaluateSwr() const {
    if (std::isnan(settleSwr_)) {
        ESP_LOGW(TAG, "kein SWR-Messwert erhalten");
        return Result::Ok;
    }
    if (cfg_.swrMax > 0.0f && settleSwr_ > cfg_.swrMax) return Result::SwrHigh;
    return Result::Ok;
}

void Tuner::setStart(bool active) {
    startActive_ = active;
    gpio_set_level(hw::PIN_ATU_START, active ? hw::START_ACTIVE : !hw::START_ACTIVE);
}

void Tuner::readKey(uint32_t now) {
    const bool raw = keyLevelActive();
    if (raw != keyRaw_) {
        keyRaw_ = raw;
        keyChangedMs_ = now;
    } else if (raw != keyStable_ && now - keyChangedMs_ >= KEY_DEBOUNCE_MS) {
        keyStable_ = raw;
    }
}

const char* Tuner::stateId(State s) {
    switch (s) {
        case State::Idle: return "idle";
        case State::WaitKey: return "wait_key";
        case State::Tuning: return "tuning";
        case State::Settle: return "settle";
        case State::Stopping: return "stopping";
    }
    return "?";
}

const char* Tuner::resultId(Result r) {
    switch (r) {
        case Result::None: return "none";
        case Result::Ok: return "ok";
        case Result::SwrHigh: return "swr_high";
        case Result::TunerFailed: return "tuner_failed";
        case Result::NoResponse: return "no_response";
        case Result::Timeout: return "timeout";
        case Result::Aborted: return "aborted";
        case Result::StopFailed: return "stop_failed";
    }
    return "?";
}
