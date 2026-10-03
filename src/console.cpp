#include "console.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "app_util.h"
#include "driver/uart.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "network.h"

namespace console {
namespace {

constexpr const char* TAG = "console";
constexpr uint16_t TELNET_PORT = 23;
constexpr size_t MAX_LINE = 128;
constexpr size_t MAX_LOG_BUFFER = 4096;
constexpr int AUTH_ATTEMPTS = 3;
constexpr uint32_t AUTH_FAIL_DELAY_MS = 1000;
constexpr uint32_t IDLE_TIMEOUT_MS = 15 * 60 * 1000;
constexpr int SEND_TIMEOUT_S = 2;
constexpr UBaseType_t TASK_PRIORITY = 2;  // unter HTTP-Server, TCI und Tuner
constexpr uint32_t TASK_STACK = 6144;

Settings* cfg = nullptr;
TciClient* tci = nullptr;
Tuner* tuner = nullptr;
WebUi* web = nullptr;

// Serielle Konsole und Telnet teilen sich die ungespeicherten Änderungen;
// Befehle laufen deshalb nacheinander.
std::mutex cmdMutex;
Settings pending;
bool unsaved = false;

// --- Ausgabe ---

class Out {
public:
    virtual ~Out() = default;
    virtual void write(const char* s, size_t n) = 0;

    void puts(const char* s) { write(s, strlen(s)); }

    void printf(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
        char buf[256];
        va_list ap;
        va_start(ap, fmt);
        const int n = vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        if (n > 0) write(buf, std::min<size_t>(n, sizeof(buf) - 1));
    }
};

class SerialOut : public Out {
public:
    void write(const char* s, size_t n) override {
        fwrite(s, 1, n, stdout);
        fflush(stdout);
    }
};

// Telnet (RFC 854) erwartet CR LF als Zeilenende
class TelnetOut : public Out {
public:
    explicit TelnetOut(int fd) : fd_(fd) {}

    void write(const char* s, size_t n) override {
        size_t start = 0;
        for (size_t i = 0; i < n; ++i) {
            if (s[i] == '\n') {
                sendRaw(s + start, i - start);
                sendRaw("\r\n", 2);
                start = i + 1;
            }
        }
        sendRaw(s + start, n - start);
    }

    void sendRaw(const char* s, size_t n) {
        while (n > 0 && !failed_) {
            const int r = send(fd_, s, n, 0);
            if (r <= 0) {
                failed_ = true;  // Client liest nicht mehr oder ist weg
                return;
            }
            s += r;
            n -= r;
        }
    }

    bool failed() const { return failed_; }

private:
    int fd_;
    bool failed_ = false;
};

// --- Log-Ausgaben in die Telnet-Sitzung ('log on') ---
//
// Der Hook läuft in dem Task, der loggt (auch im Tuner-Task). Er schreibt nur in
// einen Puffer; gesendet wird im Telnet-Task, damit niemand am Netzwerk hängt.

vprintf_like_t serialVprintf = nullptr;
std::atomic<bool> logToTelnet{false};
std::mutex logMutex;
std::string logBuffer;
bool logDropped = false;

int logHook(const char* fmt, va_list ap) {
    if (logToTelnet.load()) {
        va_list copy;
        va_copy(copy, ap);
        char buf[200];
        const int n = vsnprintf(buf, sizeof(buf), fmt, copy);
        va_end(copy);
        if (n > 0) {
            std::lock_guard<std::mutex> lock(logMutex);
            if (logBuffer.size() + n < MAX_LOG_BUFFER) {
                logBuffer.append(buf, std::min<size_t>(n, sizeof(buf) - 1));
            } else {
                logDropped = true;
            }
        }
    }
    return serialVprintf(fmt, ap);
}

// --- Hilfsfunktionen ---

std::string trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t");
    const size_t b = s.find_last_not_of(" \t");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

std::vector<std::string> split(const std::string& s) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && s[i] == ' ') ++i;
        const size_t start = i;
        while (i < s.size() && s[i] != ' ') ++i;
        if (i > start) out.push_back(s.substr(start, i - start));
    }
    return out;
}

bool parseLong(const std::string& s, long lo, long hi, long& out) {
    if (s.empty()) return false;
    char* end = nullptr;
    const long v = strtol(s.c_str(), &end, 10);
    if (*end != '\0' || v < lo || v > hi) return false;
    out = v;
    return true;
}

std::string uptime(uint32_t s) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%lud %02lu:%02lu:%02lu", static_cast<unsigned long>(s / 86400),
             static_cast<unsigned long>(s / 3600 % 24), static_cast<unsigned long>(s / 60 % 60),
             static_cast<unsigned long>(s % 60));
    return buf;
}

Settings currentSettings() {
    std::lock_guard<std::recursive_mutex> lock(appMutex());
    return *cfg;
}

// Einstellungen, wie sie nach 'save' gelten würden
Settings view() { return unsaved ? pending : currentSettings(); }

Settings& edit() {
    if (!unsaved) pending = currentSettings();
    unsaved = true;
    return pending;
}

// --- Befehle ---

struct Session {
    bool telnet;
    bool quit = false;
};

void help(Out& io, const Session& s) {
    io.puts(
        "\nCommands:\n"
        "  show                     configuration and state\n"
        "  history                  last tuning runs\n"
        "  scan                     scan for Wi-Fi networks\n"
        "  ssid <name>              Wi-Fi network\n"
        "  pass <secret>            Wi-Fi password ('pass -' = none)\n"
        "  hostname <name>          name for DHCP and mDNS (<name>.local)\n"
        "  tci <host> [port]        TCI server of the SDR software ('tci -' = none)\n"
        "  trx <all|0-3>            react to TUNE of this transceiver\n"
        "  keyactive <high|low>     active level of the KEY input\n"
        "  starthold <ms>           hold START after KEY becomes active\n"
        "  keywait <ms>             wait for KEY after START\n"
        "  timeout <ms>             maximum tune duration, then carrier off\n"
        "  swrsettle <ms>           carrier after tuning for the SWR reading (0 = off)\n"
        "  swrmax <value>           maximum SWR after tuning (0 = no check)\n");
    io.puts(s.telnet ? "  webpass <current> <new|off>  web and telnet password\n"
                     : "  webpass <new|off>        web and telnet password\n");
    io.puts(
        "  save                     store and apply (restarts if Wi-Fi or name changed)\n"
        "  discard                  drop unsaved changes\n"
        "  tune                     start a tune (sends a carrier!)\n"
        "  stop                     abort the tune\n"
        "  reboot                   restart (waits for a running tune to end)\n"
        "  factory yes              erase all settings and restart\n");
    if (s.telnet) {
        io.puts(
            "  log <on|off>             show the log output in this session\n"
            "  quit                     close the session\n");
    }
    io.puts("  help\n");
}

void show(Out& io) {
    const Settings c = view();
    const Tuner::Snapshot t = tuner->snapshot();

    io.printf("\nFirmware  %s, uptime %s\n", esp_app_get_description()->version, uptime(millis() / 1000).c_str());
    io.printf("Name      %s  ->  http://%s.local/\n", c.hostname.c_str(), c.hostname.c_str());
    io.printf("Wi-Fi     SSID '%s'  password %s\n", c.wifiSsid.c_str(), c.wifiPass.empty() ? "-" : "set");
    if (net::staConnected()) {
        io.printf("          connected to '%s', IP %s, RSSI %d dBm\n", net::staSsid().c_str(), net::staIp().c_str(),
                  net::staRssi());
    } else {
        io.puts("          not connected\n");
    }
    if (net::apActive()) io.printf("          access point '%s' active, 192.168.4.1\n", net::apSsid().c_str());

    if (c.tciHost.empty()) {
        io.puts("TCI       no server configured\n");
    } else {
        const char* state = tci->ready() ? "ready" : tci->connected() ? "connected, waiting for READY" : "not connected";
        io.printf("TCI       %s:%u  %s", c.tciHost.c_str(), c.tciPort, state);
        if (tci->ready()) io.printf("  (%s, %s)", tci->device().c_str(), tci->protocol().c_str());
        io.puts("\n");
    }

    io.printf("Tuner     %s", Tuner::stateId(t.state));
    if (t.state != Tuner::State::Idle) io.printf(" TRX %d, %.1f s", t.trx, t.elapsedMs / 1000.0);
    io.printf(", KEY %s%s\n", t.key ? "active" : "inactive", tuner->locked() ? ", locked (update/restart/scan)" : "");
    char trx[8];
    snprintf(trx, sizeof(trx), "%d", c.tuneTrx);
    io.printf("          TRX %s, KEY active %s, START hold %u ms, KEY wait %u ms, timeout %u ms\n",
              c.tuneTrx < 0 ? "all" : trx, c.keyActiveHigh ? "high" : "low", c.startHoldMs, c.keyWaitMs,
              c.tuneTimeoutMs);
    if (c.swrMax > 0.0f) {
        io.printf("          SWR reading %u ms, max SWR %.1f\n", c.swrSettleMs, c.swrMax);
    } else {
        io.printf("          SWR reading %u ms, no SWR check\n", c.swrSettleMs);
    }
    io.printf("Web       password %s\n", c.webPass.empty() ? "- (web and telnet open)" : "set");
    if (unsaved) io.puts("\nUnsaved changes - 'save' stores them, 'discard' drops them.\n");
}

void history(Out& io) {
    const Tuner::Snapshot t = tuner->snapshot();
    if (t.historyCount == 0) {
        io.puts("no tuning runs yet\n");
        return;
    }
    const uint32_t now = millis();
    io.puts("     ago  TRX  frequency     result         SWR  duration\n");
    for (size_t i = 0; i < t.historyCount; ++i) {
        const Tuner::Record& r = t.history[i];
        char swr[8] = "-";
        if (!std::isnan(r.swr)) snprintf(swr, sizeof(swr), "%.2f", r.swr);
        io.printf("%7lus  %3d  %8.3f MHz  %-13s %5s  %5.1f s\n", static_cast<unsigned long>((now - r.finishedMs) / 1000),
                  r.trx, r.freqHz / 1e6, Tuner::resultId(r.result), swr, r.durationMs / 1000.0);
    }
}

void scan(Out& io) {
    if (!tuner->lockIfIdle(Tuner::LOCK_SCAN)) {
        io.puts("not while tuning\n");
        return;
    }
    io.puts("scanning...\n");
    std::vector<net::Network> networks;
    const bool ok = net::scan(networks);
    tuner->setLock(Tuner::LOCK_SCAN, false);
    if (!ok) {
        io.puts("scan failed\n");
        return;
    }
    if (networks.empty()) io.puts("no networks found\n");
    for (const net::Network& n : networks) {
        io.printf("  %4d dBm  ch %2u  %-6s %s\n", n.rssi, n.channel, n.secure ? "" : "open", n.ssid.c_str());
    }
}

// Zahlenwert in ein Feld der ungespeicherten Einstellungen
template <typename T>
void setNumber(Out& io, const std::string& arg, long lo, long hi, T Settings::*field, const char* unit) {
    long v;
    if (!parseLong(arg, lo, hi, v)) {
        io.printf("value %ld..%ld%s expected\n", lo, hi, unit);
        return;
    }
    Settings& s = edit();
    s.*field = static_cast<T>(v);
    s.sanitize();
    io.printf("set to %ld%s (not saved yet)\n", static_cast<long>(s.*field), unit);
}

void execute(const std::string& input, Out& io, Session& session) {
    const std::string line = trim(input);
    if (line.empty()) return;
    const size_t sp = line.find(' ');
    std::string cmd = line.substr(0, sp);
    for (char& ch : cmd) ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
    const std::string rest = sp == std::string::npos ? std::string() : trim(line.substr(sp + 1));
    const std::vector<std::string> args = split(rest);

    if (cmd == "help" || cmd == "?") {
        help(io, session);
    } else if (cmd == "show") {
        show(io);
    } else if (cmd == "history") {
        history(io);
    } else if (cmd == "scan") {
        scan(io);
    } else if (cmd == "ssid") {
        if (rest.empty()) return io.puts("ssid <name>\n");
        edit().wifiSsid = rest;
        io.puts("Wi-Fi network set (not saved yet)\n");
    } else if (cmd == "pass") {
        if (rest.empty()) return io.puts("pass <secret>, or 'pass -' for none\n");
        edit().wifiPass = rest == "-" ? std::string() : rest;
        io.puts("Wi-Fi password set (not saved yet)\n");
    } else if (cmd == "hostname") {
        if (args.size() != 1) return io.puts("hostname <name>\n");
        Settings& s = edit();
        s.hostname = args[0];
        s.sanitize();
        io.printf("name '%s' (not saved yet)\n", s.hostname.c_str());
    } else if (cmd == "tci") {
        long port = 0;
        if (args.empty() || args.size() > 2 || (args.size() == 2 && !parseLong(args[1], 1, 65535, port))) {
            return io.puts("tci <host> [port], or 'tci -' for none\n");
        }
        Settings& s = edit();
        s.tciHost = args[0] == "-" ? std::string() : args[0];
        if (port) s.tciPort = static_cast<uint16_t>(port);
        io.printf("TCI server %s:%u (not saved yet)\n", s.tciHost.empty() ? "-" : s.tciHost.c_str(), s.tciPort);
    } else if (cmd == "trx") {
        long v = -1;
        if (args.size() != 1 || (args[0] != "all" && !parseLong(args[0], 0, 3, v))) return io.puts("trx <all|0-3>\n");
        edit().tuneTrx = static_cast<int8_t>(v);
        io.puts("transceiver set (not saved yet)\n");
    } else if (cmd == "keyactive") {
        if (args.size() != 1 || (args[0] != "high" && args[0] != "low")) return io.puts("keyactive <high|low>\n");
        edit().keyActiveHigh = args[0] == "high";
        io.puts("KEY level set (not saved yet)\n");
    } else if (cmd == "starthold") {
        setNumber(io, rest, 50, 2000, &Settings::startHoldMs, " ms");
    } else if (cmd == "keywait") {
        setNumber(io, rest, 500, 10000, &Settings::keyWaitMs, " ms");
    } else if (cmd == "timeout") {
        setNumber(io, rest, 1000, 60000, &Settings::tuneTimeoutMs, " ms");
    } else if (cmd == "swrsettle") {
        setNumber(io, rest, 0, 2000, &Settings::swrSettleMs, " ms");
    } else if (cmd == "swrmax") {
        char* end = nullptr;
        const float v = rest.empty() ? NAN : strtof(rest.c_str(), &end);
        if (std::isnan(v) || *end != '\0' || v < 0.0f || v > 10.0f) return io.puts("swrmax <1.0..10.0>, 0 = no check\n");
        Settings& s = edit();
        s.swrMax = v;
        s.sanitize();
        io.printf("max SWR %.1f (not saved yet)\n", s.swrMax);
    } else if (cmd == "webpass") {
        // Wer das Passwort ändert, kann auch Firmware einspielen. Telnet hat sich zwar
        // angemeldet, aber wie bei juma muss das aktuelle Passwort mitkommen.
        const std::string current = currentSettings().webPass;
        const size_t want = session.telnet && !current.empty() ? 2 : 1;
        if (args.size() != want) {
            return io.puts(want == 2 ? "webpass <current> <new|off>\n" : "webpass <new|off>\n");
        }
        if (want == 2 && args[0] != current) return io.puts("current password wrong\n");
        edit().webPass = args.back() == "off" ? std::string() : args.back();
        io.puts("password set (not saved yet)\n");
    } else if (cmd == "save") {
        if (!unsaved) return io.puts("nothing to save\n");
        bool reboot;
        {
            std::lock_guard<std::recursive_mutex> lock(appMutex());
            reboot = web->applySettings(pending);
        }
        unsaved = false;
        io.puts(reboot ? "saved, restarting...\n" : "saved and applied\n");
    } else if (cmd == "discard") {
        unsaved = false;
        io.puts("unsaved changes dropped\n");
    } else if (cmd == "tune") {
        if (!tci->ready()) return io.puts("no TCI connection\n");
        if (tuner->locked()) return io.puts("locked (update, restart or scan)\n");
        if (tuner->busy()) return io.puts("already tuning\n");
        tuner->requestStart();
        io.puts("tune requested\n");
    } else if (cmd == "stop") {
        if (!tuner->busy()) return io.puts("not tuning\n");
        tuner->requestStop();
        io.puts("stop requested\n");
    } else if (cmd == "reboot") {
        io.puts(tuner->busy() ? "restarting after the tune has ended...\n" : "restarting...\n");
        web->scheduleReboot();
    } else if (cmd == "factory") {
        if (rest != "yes") return io.puts("factory yes  - erases all settings, including Wi-Fi\n");
        Settings::erase();
        unsaved = false;
        io.puts("settings erased, restarting...\n");
        web->scheduleReboot();
    } else if (cmd == "log" && session.telnet) {
        if (args.size() != 1 || (args[0] != "on" && args[0] != "off")) return io.puts("log <on|off>\n");
        logToTelnet = args[0] == "on";
        io.puts(logToTelnet ? "log output on\n" : "log output off\n");
    } else if (cmd == "quit" && session.telnet) {
        session.quit = true;
    } else {
        io.puts("unknown command - 'help' lists the commands\n");
    }
}

void run(const std::string& line, Out& io, Session& session) {
    std::lock_guard<std::mutex> lock(cmdMutex);
    execute(line, io, session);
}

// --- Serielle Konsole ---

void serialTask(void*) {
    if (uart_driver_install(UART_NUM_0, 256, 0, 0, nullptr, 0) != ESP_OK) {
        ESP_LOGE(TAG, "UART0 nicht verfügbar, keine serielle Konsole");
        vTaskDelete(nullptr);
    }
    SerialOut io;
    Session session{false};
    std::string line;
    bool lastCr = false;
    for (;;) {
        uint8_t c;
        if (uart_read_bytes(UART_NUM_0, &c, 1, portMAX_DELAY) != 1) continue;
        if (c == '\r' || c == '\n') {
            const bool skip = c == '\n' && lastCr;  // CR LF als ein Zeilenende
            lastCr = c == '\r';
            if (skip) continue;
            io.puts("\n");
            run(line, io, session);
            line.clear();
            io.puts("> ");
        } else if (c == 8 || c == 127) {
            lastCr = false;
            if (!line.empty()) {
                line.pop_back();
                io.puts("\b \b");
            }
        } else if (c >= 32 && c < 127 && line.size() < MAX_LINE) {
            lastCr = false;
            line += static_cast<char>(c);
            io.write(reinterpret_cast<const char*>(&c), 1);  // Terminals echoen nicht selbst
        }
    }
}

// --- Telnet ---

// Entfernt Telnet-Steuersequenzen (IAC ...) und setzt Zeilen zusammen
class LineReader {
public:
    // Liefert true, wenn eine Zeile fertig ist (dann in line())
    bool feed(uint8_t b) {
        switch (state_) {
            case State::Data:
                if (b == IAC) {
                    state_ = State::Iac;
                    return false;
                }
                return text(b);
            case State::Iac:
                if (b == IAC) {
                    state_ = State::Data;
                    return text(b);
                }
                state_ = (b >= WILL && b <= DONT) ? State::Option : b == SB ? State::Sub : State::Data;
                return false;
            case State::Option:
                state_ = State::Data;
                return false;
            case State::Sub:
                if (b == IAC) state_ = State::SubIac;
                return false;
            case State::SubIac:
                state_ = b == SE ? State::Data : State::Sub;
                return false;
        }
        return false;
    }

    std::string take() {
        std::string l;
        l.swap(line_);
        return l;
    }

    static constexpr uint8_t IAC = 255, WILL = 251, WONT = 252, DO = 253, DONT = 254, SB = 250, SE = 240, ECHO = 1;

private:
    enum class State : uint8_t { Data, Iac, Option, Sub, SubIac };

    bool text(uint8_t b) {
        if (b == '\r' || b == '\n') {
            const bool skip = b == '\n' && lastCr_;
            lastCr_ = b == '\r';
            return !skip;
        }
        lastCr_ = false;
        if ((b == 8 || b == 127) && !line_.empty()) {
            line_.pop_back();
        } else if (b >= 32 && b < 127 && line_.size() < MAX_LINE) {
            line_ += static_cast<char>(b);
        }
        return false;
    }

    State state_ = State::Data;
    bool lastCr_ = false;
    std::string line_;
};

void setEcho(TelnetOut& io, bool serverEchoes) {
    const char seq[] = {static_cast<char>(LineReader::IAC),
                        static_cast<char>(serverEchoes ? LineReader::WILL : LineReader::WONT),
                        static_cast<char>(LineReader::ECHO)};
    io.sendRaw(seq, sizeof(seq));
}

// Wartet auf Daten der Sitzung; weitere Verbindungen werden abgewiesen
enum class Wait : uint8_t { Data, Timeout, Error };

Wait waitInput(int fd, int listenFd, uint32_t timeoutMs) {
    fd_set rd;
    FD_ZERO(&rd);
    FD_SET(fd, &rd);
    FD_SET(listenFd, &rd);
    timeval tv = {static_cast<time_t>(timeoutMs / 1000), static_cast<suseconds_t>(timeoutMs % 1000 * 1000)};
    const int n = select(std::max(fd, listenFd) + 1, &rd, nullptr, nullptr, &tv);
    if (n < 0) return Wait::Error;
    if (FD_ISSET(listenFd, &rd)) {
        const int other = accept(listenFd, nullptr, nullptr);
        if (other >= 0) {
            const char msg[] = "another telnet session is active\r\n";
            send(other, msg, sizeof(msg) - 1, 0);
            close(other);
        }
    }
    return FD_ISSET(fd, &rd) ? Wait::Data : Wait::Timeout;
}

// Liest eine Zeile (für die Anmeldung)
bool readLine(int fd, int listenFd, LineReader& reader, std::string& line) {
    const uint32_t start = millis();
    for (;;) {
        if (millis() - start > 60000) return false;
        const Wait w = waitInput(fd, listenFd, 1000);
        if (w == Wait::Error) return false;
        if (w == Wait::Timeout) continue;
        uint8_t buf[64];
        const int n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) return false;
        for (int i = 0; i < n; ++i) {
            if (reader.feed(buf[i])) {
                line = reader.take();
                return true;
            }
        }
    }
}

bool authenticate(int fd, int listenFd, TelnetOut& io, LineReader& reader) {
    const std::string password = currentSettings().webPass;
    if (password.empty()) return true;
    for (int attempt = 0; attempt < AUTH_ATTEMPTS; ++attempt) {
        io.puts("Password: ");
        setEcho(io, true);  // Client zeigt die Eingabe nicht an
        std::string line;
        const bool got = readLine(fd, listenFd, reader, line);
        setEcho(io, false);
        io.puts("\n");
        if (!got || io.failed()) return false;
        if (line == password) return true;
        vTaskDelay(pdMS_TO_TICKS(AUTH_FAIL_DELAY_MS));
        io.puts("wrong password\n");
    }
    return false;
}

void drainLog(TelnetOut& io) {
    std::string text;
    bool dropped;
    {
        std::lock_guard<std::mutex> lock(logMutex);
        text.swap(logBuffer);
        dropped = logDropped;
        logDropped = false;
    }
    if (dropped) io.puts("[log output dropped]\n");
    if (!text.empty()) io.write(text.data(), text.size());
}

void session(int fd, int listenFd) {
    const timeval tv = {SEND_TIMEOUT_S, 0};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    const int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    TelnetOut io(fd);
    LineReader reader;
    io.printf("\nTCI to ICOM Tuner Interface %s\n", esp_app_get_description()->version);
    if (!authenticate(fd, listenFd, io, reader)) return;
    ESP_LOGI(TAG, "Telnet-Sitzung geöffnet");
    io.puts("'help' lists the commands\n> ");

    Session s{true};
    uint32_t lastInput = millis();
    while (!s.quit && !io.failed()) {
        if (millis() - lastInput > IDLE_TIMEOUT_MS) {
            io.puts("\nidle timeout\n");
            break;
        }
        const Wait w = waitInput(fd, listenFd, 100);
        if (logToTelnet) drainLog(io);
        if (w == Wait::Error) break;
        if (w == Wait::Timeout) continue;
        uint8_t buf[64];
        const int n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        lastInput = millis();
        for (int i = 0; i < n && !s.quit; ++i) {
            if (!reader.feed(buf[i])) continue;
            run(reader.take(), io, s);
            if (!s.quit) io.puts("> ");
        }
    }
    logToTelnet = false;
    {
        std::lock_guard<std::mutex> lock(logMutex);
        logBuffer.clear();
    }
    ESP_LOGI(TAG, "Telnet-Sitzung beendet");
}

void telnetTask(void*) {
    const int listenFd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    const int one = 1;
    setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(TELNET_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (listenFd < 0 || bind(listenFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        listen(listenFd, 1) != 0) {
        ESP_LOGE(TAG, "Telnet-Port %u nicht verfügbar", TELNET_PORT);
        if (listenFd >= 0) close(listenFd);
        vTaskDelete(nullptr);
    }
    for (;;) {
        const int fd = accept(listenFd, nullptr, nullptr);
        if (fd < 0) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        session(fd, listenFd);
        shutdown(fd, SHUT_RDWR);
        close(fd);
    }
}

}  // namespace

void begin(Settings& settings, TciClient& tciClient, Tuner& t, WebUi& webUi) {
    cfg = &settings;
    tci = &tciClient;
    tuner = &t;
    web = &webUi;
    serialVprintf = esp_log_set_vprintf(logHook);
    xTaskCreate(serialTask, "console", TASK_STACK, nullptr, TASK_PRIORITY, nullptr);
    xTaskCreate(telnetTask, "telnet", TASK_STACK, nullptr, TASK_PRIORITY, nullptr);
}

}  // namespace console
