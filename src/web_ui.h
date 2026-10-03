#pragma once

#include <atomic>

#include "esp_http_server.h"
#include "settings.h"
#include "tci_client.h"
#include "tuner.h"

// Weboberfläche: Status, Abstimmungsverlauf, Einstellungen und Firmware-Update.
// Die Handler laufen im Task des HTTP-Servers und sperren dafür appMutex().
class WebUi {
public:
    WebUi(Settings& cfg, TciClient& tci, Tuner& tuner) : cfg_(cfg), tci_(tci), tuner_(tuner) {}

    bool begin();
    bool rebootDue() const;

private:
    static esp_err_t handleIndex(httpd_req_t* req);
    static esp_err_t handleStatus(httpd_req_t* req);
    static esp_err_t handleGetSettings(httpd_req_t* req);
    static esp_err_t handlePostSettings(httpd_req_t* req);
    static esp_err_t handleReboot(httpd_req_t* req);
    static esp_err_t handleTune(httpd_req_t* req);
    static esp_err_t handleScan(httpd_req_t* req);
    static esp_err_t handleUpdate(httpd_req_t* req);
    static esp_err_t handleNotFound(httpd_req_t* req, httpd_err_code_t err);

    bool authorized(httpd_req_t* req);
    void scheduleReboot();

    Settings& cfg_;
    TciClient& tci_;
    Tuner& tuner_;
    httpd_handle_t server_ = nullptr;
    std::atomic<uint32_t> rebootAtMs_{0};
};
