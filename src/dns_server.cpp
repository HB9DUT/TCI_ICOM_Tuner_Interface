#include "dns_server.h"

#include <cstring>

#include "esp_log.h"
#include "lwip/sockets.h"

namespace {

constexpr const char* TAG = "dns";
constexpr uint16_t DNS_PORT = 53;
constexpr size_t HEADER_LEN = 12;
constexpr size_t MAX_PACKET = 512;
constexpr uint16_t TYPE_A = 1;
constexpr uint16_t CLASS_IN = 1;
constexpr uint32_t TTL_S = 60;

uint16_t read16(const uint8_t* p) { return static_cast<uint16_t>(p[0] << 8 | p[1]); }

void write16(uint8_t* p, uint16_t v) {
    p[0] = v >> 8;
    p[1] = v & 0xFF;
}

}  // namespace

void CaptiveDns::start(uint32_t ip) {
    if (task_) return;
    ip_ = ip;
    running_ = true;
    TaskHandle_t handle = nullptr;
    xTaskCreate(task, "captive_dns", 3072, this, 5, &handle);
    task_ = handle;
}

void CaptiveDns::stop() {
    if (!task_) return;
    running_ = false;
    // Der Task prüft das Flag spätestens nach dem Empfangs-Timeout und beendet sich
    for (int i = 0; i < 20 && task_; ++i) vTaskDelay(pdMS_TO_TICKS(50));
}

void CaptiveDns::task(void* arg) {
    auto* self = static_cast<CaptiveDns*>(arg);
    self->run();
    self->task_ = nullptr;
    vTaskDelete(nullptr);
}

void CaptiveDns::run() {
    const int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Socket konnte nicht angelegt werden");
        return;
    }
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(DNS_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "Port %u belegt", DNS_PORT);
        close(sock);
        return;
    }
    timeval tv = {0, 500 * 1000};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    uint8_t buf[MAX_PACKET];
    while (running_) {
        sockaddr_in from = {};
        socklen_t fromLen = sizeof(from);
        const int len = recvfrom(sock, buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from), &fromLen);
        if (len < static_cast<int>(HEADER_LEN)) continue;

        // Nur Anfragen (QR = 0) mit mindestens einer Frage
        if ((buf[2] & 0x80) || read16(buf + 4) == 0) continue;

        // Ende des Namens der ersten Frage suchen (Labels bis zur Null)
        size_t pos = HEADER_LEN;
        while (pos < static_cast<size_t>(len) && buf[pos] != 0) pos += buf[pos] + 1;
        pos += 1;  // Null-Label
        if (pos + 4 > static_cast<size_t>(len)) continue;
        const uint16_t qtype = read16(buf + pos);
        const uint16_t qclass = read16(buf + pos + 2);
        pos += 4;

        // Antwort: Header + erste Frage übernehmen, weitere Abschnitte verwerfen
        const bool answerA = qtype == TYPE_A && qclass == CLASS_IN && pos + 16 <= sizeof(buf);
        write16(buf + 2, 0x8180);  // Antwort, Rekursion gewünscht und verfügbar, kein Fehler
        write16(buf + 4, 1);
        write16(buf + 6, answerA ? 1 : 0);
        write16(buf + 8, 0);
        write16(buf + 10, 0);
        if (answerA) {
            uint8_t* a = buf + pos;
            write16(a, 0xC00C);  // Zeiger auf den Namen in der Frage
            write16(a + 2, TYPE_A);
            write16(a + 4, CLASS_IN);
            write16(a + 6, TTL_S >> 16);
            write16(a + 8, TTL_S & 0xFFFF);
            write16(a + 10, 4);
            memcpy(a + 12, &ip_, 4);
            pos += 16;
        }
        sendto(sock, buf, pos, 0, reinterpret_cast<sockaddr*>(&from), fromLen);
    }
    close(sock);
}
