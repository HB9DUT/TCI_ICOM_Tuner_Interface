idf_component_register(
    SRCS
        main.cpp
        settings.cpp
        tci_client.cpp
        tuner.cpp
        status_led.cpp
        network.cpp
        dns_server.cpp
        web_ui.cpp
    INCLUDE_DIRS "."
    EMBED_FILES index.html
    PRIV_REQUIRES
        driver
        esp_app_format
        esp_event
        esp_http_server
        esp_netif
        esp_timer
        esp_wifi
        json
        lwip
        mbedtls
        nvs_flash
        espressif__esp_websocket_client
        espressif__mdns
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra)
