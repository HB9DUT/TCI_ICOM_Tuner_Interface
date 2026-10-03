#include "tuner.h"

#include <algorithm>
#include <cinttypes>

#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/task.h"
#include "hw_config.h"
#include "soc/gpio_reg.h"
#include "soc/soc.h"

namespace {

constexpr const char* TAG = "tuner";
constexpr uint32_t LOOP_MS = 2;
constexpr UBaseType_t TASK_PRIORITY = 10;  // über HTTP-Server und TCI
constexpr BaseType_t TASK_CORE = 1;        // WLAN läuft auf Core 0
constexpr uint32_t TASK_STACK = 6144;
constexpr UBaseType_t COMMAND_QUEUE_LEN = 16;
constexpr UBaseType_t KEY_QUEUE_LEN = 64;
constexpr TickType_t POST_TIMEOUT = pdMS_TO_TICKS(10);

constexpr uint32_t KEY_DEBOUNCE_MS = 5;
// Wird KEY innerhalb dieser Zeit nach der Freigabe erneut aktiv, meldet der Tuner einen Fehlschlag
constexpr uint32_t FAIL_PULSE_WINDOW_MS = 100;
constexpr uint16_t TX_SENSORS_INTERVAL_MS = 100;
constexpr uint32_t STOP_RETRY_MS = 500;
constexpr uint8_t STOP_MAX_ATTEMPTS = 6;
// Träger vor START: so lange höchstens auf TRX:false und das TUNE:false-Echo warten
constexpr uint32_t CARRIER_OFF_TIMEOUT_MS = 1500;
// Thetis meldet TUNE:false bis zu ~500 ms verspätet, auch nach einem neuen TUNE:true
constexpr uint32_t TUNE_OFF_ECHO_MS = 1000;

// KEY-Flanken aus dem Interrupt. Der Handler liegt im IRAM und liest das
// GPIO-Register direkt, damit er auch während Flash-Zugriffen (z.B. Update)
// sofort läuft und den Zeitpunkt der Flanke festhält.
static_assert(hw::PIN_ATU_KEY < 32, "KEY muss auf GPIO 0..31 liegen (GPIO_IN_REG)");

struct KeyEdge {
    int64_t us;
    uint8_t level;
};

QueueHandle_t keyEdges = nullptr;
volatile bool keyOverflow = false;

void IRAM_ATTR keyIsr(void*) {
    const KeyEdge e = {esp_timer_get_time(), static_cast<uint8_t>((REG_READ(GPIO_IN_REG) >> hw::PIN_ATU_KEY) & 1)};
    BaseType_t woken = pdFALSE;
    if (xQueueSendFromISR(keyEdges, &e, &woken) != pdTRUE) keyOverflow = true;
    if (woken) portYIELD_FROM_ISR();
}

}  // namespace

Tuner::Config Tuner::Config::from(const Settings& s) {
    Config c;
    c.tuneTrx = s.tuneTrx;
    c.keyActiveHigh = s.keyActiveHigh;
    c.startHoldMs = s.startHoldMs;
    c.keyWaitMs = s.keyWaitMs;
    c.tuneTimeoutMs = s.tuneTimeoutMs;
    c.swrSettleMs = s.swrSettleMs;
    c.swrMax = s.swrMax;
    return c;
}

void Tuner::initOutput() {
    setStart(false);  // Pegel setzen, bevor der Pin Ausgang wird
    gpio_config_t out = {};
    out.pin_bit_mask = 1ULL << hw::PIN_ATU_START;
    out.mode = GPIO_MODE_OUTPUT;
    gpio_config(&out);
}

void Tuner::start(const Config& cfg) {
    cfg_ = cfg;
    commands_ = xQueueCreate(COMMAND_QUEUE_LEN, sizeof(Command));
    keyEdges = xQueueCreate(KEY_QUEUE_LEN, sizeof(KeyEdge));

    gpio_config_t in = {};
    in.pin_bit_mask = 1ULL << hw::PIN_ATU_KEY;
    in.mode = GPIO_MODE_INPUT;
    in.pull_up_en = GPIO_PULLUP_ENABLE;
    in.intr_type = GPIO_INTR_ANYEDGE;
    gpio_config(&in);
    const esp_err_t err = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) ESP_LOGE(TAG, "GPIO-Interrupt-Dienst: %s", esp_err_to_name(err));
    gpio_isr_handler_add(hw::PIN_ATU_KEY, keyIsr, nullptr);

    keyRaw_ = keyStable_ = (gpio_get_level(hw::PIN_ATU_KEY) != 0) == cfg_.keyActiveHigh;
    keyChangedMs_ = millis();
    publish(millis());

    xTaskCreatePinnedToCore(taskEntry, "tuner", TASK_STACK, this, TASK_PRIORITY, nullptr, TASK_CORE);
}

void Tuner::taskEntry(void* arg) { static_cast<Tuner*>(arg)->run(); }

void Tuner::run() {
    for (;;) {
        Command cmd;
        if (xQueueReceive(commands_, &cmd, pdMS_TO_TICKS(LOOP_MS)) == pdTRUE) {
            do {
                handle(cmd, millis());
            } while (xQueueReceive(commands_, &cmd, 0) == pdTRUE);
        }
        const uint32_t now = millis();
        processKey(now);
        update(now);
        publish(now);
    }
}

// --- Schnittstelle für andere Tasks ---

void Tuner::post(const Command& cmd) {
    if (!commands_) return;
    if (xQueueSend(commands_, &cmd, POST_TIMEOUT) != pdTRUE) ESP_LOGW(TAG, "Befehls-Queue voll");
}

void Tuner::applyConfig(const Config& cfg) {
    Command c = {};
    c.type = Command::Type::Config;
    c.cfg = cfg;
    post(c);
}

void Tuner::onTuneEvent(int trx, bool on, uint32_t freqHz, bool atConnect, bool carrierOn) {
    Command c = {};
    c.type = Command::Type::Tune;
    c.trx = trx;
    c.on = on;
    c.freqHz = freqHz;
    c.atConnect = atConnect;
    c.carrierOn = carrierOn;
    post(c);
}

void Tuner::onTxSensors(int trx, float swr) {
    Command c = {};
    c.type = Command::Type::TxSensors;
    c.trx = trx;
    c.swr = swr;
    post(c);
}

void Tuner::onTciReady(bool ready) {
    Command c = {};
    c.type = Command::Type::TciReady;
    c.on = ready;
    post(c);
}

void Tuner::requestStart() {
    Command c = {};
    c.type = Command::Type::WebStart;
    post(c);
}

void Tuner::requestStop() {
    Command c = {};
    c.type = Command::Type::WebStop;
    post(c);
}

void Tuner::setLock(Lock reason, bool on) {
    if (on) {
        locks_.fetch_or(reason);
    } else {
        locks_.fetch_and(static_cast<uint8_t>(~reason));
    }
}

// Erst sperren, dann prüfen (claimStart macht es umgekehrt): so kann keine
// Sitzung zwischen Prüfung und Sperre beginnen.
bool Tuner::lockIfIdle(Lock reason) {
    setLock(reason, true);
    if (!busy()) return true;
    setLock(reason, false);
    return false;
}

Tuner::Snapshot Tuner::snapshot() const {
    std::lock_guard<std::mutex> lock(snapshotMutex_);
    return snapshot_;
}

// --- Tuner-Task ---

void Tuner::handle(const Command& cmd, uint32_t now) {
    switch (cmd.type) {
        case Command::Type::Config:
            if (cmd.cfg.keyActiveHigh != cfg_.keyActiveHigh) {
                keyRaw_ = keyStable_ = (gpio_get_level(hw::PIN_ATU_KEY) != 0) == cmd.cfg.keyActiveHigh;
                keyChangedMs_ = now;
            }
            cfg_ = cmd.cfg;
            break;

        case Command::Type::Tune:
            if (cmd.on) {
                // Weitere TUNE:true während einer laufenden Sitzung sind Echos/Antworten.
                if (state_ != State::Idle || (cfg_.tuneTrx >= 0 && cmd.trx != cfg_.tuneTrx)) break;
                if (cmd.atConnect) {
                    // Träger war schon an, als die Verbindung aufgebaut wurde (z.B. nach einem
                    // Neustart des Moduls mitten im Tune): beenden statt abstimmen.
                    ESP_LOGW(TAG, "Tune beim Verbindungsaufbau aktiv, Träger aus");
                    tci_.setTune(cmd.trx, false);
                    break;
                }
                if (!claimStart(cmd.trx, "TCI")) {
                    tci_.setTune(cmd.trx, false);
                    break;
                }
                if (cmd.carrierOn) {
                    // Liegt der Träger schon vor START an (Thetis), stimmt der Tuner nicht ab:
                    // Träger aus, danach START und Träger wieder ein wie über die Weboberfläche.
                    ESP_LOGI(TAG, "Träger vor START, schalte ihn zuerst aus");
                    tci_.setTune(cmd.trx, false);
                    startSession(cmd.trx, cmd.freqHz, now, false);
                    waitCarrierOff_ = true;
                    break;
                }
                startSession(cmd.trx, cmd.freqHz, now);
                break;
            }
            if (state_ == State::Idle || cmd.trx != trx_) break;
            if (state_ != State::Stopping && waitCarrierOff_) {
                tuneOffSeen_ = true;  // Echo auf das eigene TUNE:false
                break;
            }
            // Verspätetes Echo kommt bei laufendem Träger; ein echter Stopp in Thetis
            // meldet zuerst TRX:false und wird nicht ignoriert.
            if (state_ != State::Stopping && cmd.carrierOn &&
                static_cast<int32_t>(now - ignoreTuneOffUntilMs_) < 0) {
                break;
            }
            if (state_ == State::Stopping) {
                finish(pendingResult_, now);
            } else {
                ESP_LOGI(TAG, "Tune im SDR-Programm beendet");
                finish(Result::Aborted, now);
            }
            break;

        case Command::Type::TxSensors:
            if (state_ == State::Idle || cmd.trx != trx_) break;
            liveSwr_ = cmd.swr;
            if (state_ == State::Settle) settleSwr_ = cmd.swr;
            break;

        case Command::Type::TciReady:
            if (!cmd.on && state_ != State::Idle) {
                ESP_LOGW(TAG, "TCI-Verbindung verloren");
                finish(Result::Aborted, now);
            }
            break;

        case Command::Type::WebStart: {
            const int trx = cfg_.tuneTrx < 0 ? 0 : cfg_.tuneTrx;
            if (state_ != State::Idle || !tci_.ready() || !claimStart(trx, "Weboberfläche")) break;
            ESP_LOGI(TAG, "Tune über die Weboberfläche");
            // Sitzung sofort beginnen statt auf das Echo zu warten: so gilt der Timeout auch,
            // falls das Echo ausbleibt. Lehnt das SDR-Programm ab, meldet es TUNE:false.
            startSession(trx, tci_.vfoHz(trx), now);
            tci_.setTune(trx, true);
            break;
        }

        case Command::Type::WebStop:
            if (state_ == State::Idle || state_ == State::Stopping) break;
            ESP_LOGI(TAG, "Abbruch über die Weboberfläche");
            requestStop(Result::Aborted, now);
            break;
    }
}

// Belegt den Tuner, sofern nichts gesperrt ist. Erst busy setzen, dann die Sperre
// prüfen (lockIfIdle macht es umgekehrt): so sieht mindestens eine Seite die
// andere, und es kann keine Sitzung neben einem Update oder Neustart beginnen.
bool Tuner::claimStart(int trx, const char* source) {
    busy_.store(true);
    if (!locked()) return true;
    busy_.store(false);
    ESP_LOGW(TAG, "Tune von %s abgelehnt (Update, Neustart oder WLAN-Suche), TRX %d", source, trx);
    return false;
}

// Übernimmt die KEY-Flanken in zeitlicher Reihenfolge. Jeder entprellte Wechsel
// geht mit seinem eigenen Zeitpunkt in die Zustandsmaschine, auch wenn mehrere
// Flanken auf einmal verarbeitet werden.
void Tuner::processKey(uint32_t now) {
    KeyEdge e;
    while (xQueueReceive(keyEdges, &e, 0) == pdTRUE) {
        const uint32_t t = static_cast<uint32_t>(e.us / 1000);
        settleKey(t);
        const bool active = (e.level != 0) == cfg_.keyActiveHigh;
        if (active != keyRaw_) {
            keyRaw_ = active;
            keyChangedMs_ = t;
        }
    }
    if (keyOverflow) {
        keyOverflow = false;
        ESP_LOGW(TAG, "KEY-Flanken verloren, Pegel neu eingelesen");
        const bool active = (gpio_get_level(hw::PIN_ATU_KEY) != 0) == cfg_.keyActiveHigh;
        if (active != keyRaw_) {
            keyRaw_ = active;
            keyChangedMs_ = now;
        }
    }
    settleKey(now);
}

void Tuner::settleKey(uint32_t t) {
    if (keyRaw_ != keyStable_ && static_cast<int32_t>(t - keyChangedMs_) >= static_cast<int32_t>(KEY_DEBOUNCE_MS)) {
        keyStable_ = keyRaw_;
        onKeyChange(keyStable_, keyChangedMs_ + KEY_DEBOUNCE_MS);
    }
}

void Tuner::onKeyChange(bool active, uint32_t t) {
    // Eine Flanke vor Sitzungsbeginn zählt ab Sitzungsbeginn
    if (state_ != State::Idle && static_cast<int32_t>(t - startMs_) < 0) t = startMs_;
    switch (state_) {
        case State::Idle:
            if (active) ESP_LOGW(TAG, "KEY aktiv ohne Tune-Anforderung");
            break;

        case State::WaitKey:
            if (active && !waitCarrierOff_) {
                ESP_LOGI(TAG, "stimmt ab (KEY nach %" PRIu32 " ms)", t - startMs_);
                keyActiveMs_ = t;
                setState(State::Tuning);
            }
            break;

        case State::Tuning:
            if (!active) {
                ESP_LOGI(TAG, "KEY frei nach %" PRIu32 " ms", t - startMs_);
                setStart(false);
                tuneEndMs_ = t;
                settleSwr_ = NAN;
                setState(State::Settle);
            }
            break;

        case State::Settle:
            if (active && t - tuneEndMs_ <= FAIL_PULSE_WINDOW_MS) {
                ESP_LOGW(TAG, "Tuner meldet Fehlschlag");
                requestStop(Result::TunerFailed, t);
            }
            break;

        case State::Stopping:
            break;
    }
}

void Tuner::update(uint32_t now) {
    const uint32_t elapsed = now - startMs_;
    switch (state_) {
        case State::Idle:
            break;

        case State::WaitKey:
            if (waitCarrierOff_) {
                // Thetis übernimmt TUNE:true erst nach dem TUNE:false-Echo (~500 ms);
                // vorher sendet es ohne Tune-Träger.
                const bool off = tuneOffSeen_ && !tci_.transmitting(trx_);
                if (!off && elapsed < CARRIER_OFF_TIMEOUT_MS) break;
                if (!off) ESP_LOGW(TAG, "Träger nicht aus, START trotzdem");
                waitCarrierOff_ = false;
                startMs_ = now;
                ignoreTuneOffUntilMs_ = now + TUNE_OFF_ECHO_MS;
                setStart(true);
                tci_.setTune(trx_, true);
                break;
            }
            // KEY war schon vor START aktiv (keine Flanke mehr): gilt als Antwort
            if (keyStable_) {
                onKeyChange(true, now);
            } else if (elapsed >= cfg_.keyWaitMs) {
                ESP_LOGW(TAG, "keine Antwort auf START");
                requestStop(Result::NoResponse, now);
            }
            break;

        case State::Tuning:
            if (startActive_ && now - keyActiveMs_ >= cfg_.startHoldMs) setStart(false);
            if (elapsed >= cfg_.tuneTimeoutMs) {
                ESP_LOGW(TAG, "Timeout nach %" PRIu32 " ms", elapsed);
                requestStop(Result::Timeout, now);
            }
            break;

        case State::Settle:
            // Erst nach dem Fehlschlag-Fenster ist sicher, dass der Tuner erfolgreich war
            if (now - tuneEndMs_ >= std::max<uint32_t>(cfg_.swrSettleMs, FAIL_PULSE_WINDOW_MS)) {
                requestStop(cfg_.swrSettleMs > 0 ? evaluateSwr() : Result::Ok, now);
            }
            break;

        case State::Stopping:
            if (now - stopSentMs_ >= STOP_RETRY_MS) {
                if (stopAttempts_ >= STOP_MAX_ATTEMPTS) {
                    finish(Result::StopFailed, now);
                } else {
                    sendStop(now);
                }
            }
            break;
    }
}

void Tuner::publish(uint32_t now) {
    std::lock_guard<std::mutex> lock(snapshotMutex_);
    Snapshot& s = snapshot_;
    const bool active = state_ != State::Idle;
    s.state = state_;
    s.key = keyStable_;
    s.trx = trx_;
    s.elapsedMs = active ? now - startMs_ : 0;
    s.liveSwr = active ? liveSwr_ : NAN;
    s.lastFailed = lastResult_ != Result::None && lastResult_ != Result::Ok && lastResult_ != Result::Aborted;
    s.lastFinishedMs = lastFinishedMs_;
    s.historyCount = historyCount_;
    for (size_t i = 0; i < historyCount_; ++i) {
        s.history[i] = history_[(historyHead_ + HISTORY_SIZE - 1 - i) % HISTORY_SIZE];
    }
}

void Tuner::startSession(int trx, uint32_t freqHz, uint32_t now, bool start) {
    trx_ = trx;
    freqHz_ = freqHz;
    startMs_ = now;
    liveSwr_ = NAN;
    settleSwr_ = NAN;
    pendingResult_ = Result::None;
    waitCarrierOff_ = tuneOffSeen_ = false;
    ignoreTuneOffUntilMs_ = now;
    ESP_LOGI(TAG, "Tune-Anforderung TRX %d, %.3f MHz", trx, freqHz_ / 1e6);
    tci_.setTxSensors(true, TX_SENSORS_INTERVAL_MS);
    setStart(start);
    setState(State::WaitKey);
}

void Tuner::requestStop(Result result, uint32_t now) {
    setStart(false);
    pendingResult_ = result;
    stopAttempts_ = 0;
    setState(State::Stopping);
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

void Tuner::finish(Result result, uint32_t now) {
    setStart(false);
    tci_.setTxSensors(false);

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
    trx_ = -1;
    setState(State::Idle);
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

void Tuner::setState(State s) {
    state_ = s;
    busy_.store(s != State::Idle);
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
