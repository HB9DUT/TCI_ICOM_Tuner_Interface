#pragma once

#include <atomic>
#include <cstdint>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Minimaler DNS-Server für das Captive Portal: beantwortet jede A-Anfrage
// mit der IP-Adresse des Access-Points.
class CaptiveDns {
public:
    void start(uint32_t ip);  // IPv4 in Netzwerk-Byte-Reihenfolge
    void stop();

private:
    static void task(void* arg);
    void run();

    uint32_t ip_ = 0;
    std::atomic<bool> running_{false};
    std::atomic<TaskHandle_t> task_{nullptr};
};
