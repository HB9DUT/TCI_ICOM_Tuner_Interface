# TCI to ICOM Tuner Interface

ESP32-Firmware, die einen automatischen Antennentuner mit **ICOM-AH-4-Schnittstelle** an ein SDR mit **ExpertSDR3** anbindet. Drückt man in ExpertSDR3 auf TUNE, startet das Interface den Tuner, wartet auf das Ende der Abstimmung, prüft das SWR und schaltet den Tune-Träger wieder ab.

Das Interface übernimmt dabei gegenüber dem Tuner die Rolle eines ICOM-Funkgeräts. Es eignet sich damit für den ICOM AH-4 und kompatible Tuner wie den Stockcorner. Die Verbindung zu ExpertSDR3 läuft über das [TCI-Protokoll](https://github.com/ExpertSDR3/TCI) (WebSocket) im WLAN, es sind also keine Kabel zum SDR nötig.

## Funktionen

- **Auslöser ist TUNE in ExpertSDR3:** Die Abstimmung startet automatisch, die Verbindung baut sich nach einem Abbruch selbst wieder auf.
- **AH-4-Ablauf:** START halten, bis der Tuner KEY setzt; das Ende der Abstimmung wird erkannt, ebenso die Fehlschlag-Meldung des Tuners.
- **Sicherheit:** Der Tune-Träger wird spätestens nach einem einstellbaren Timeout abgeschaltet, auch wenn der Tuner nicht antwortet. Das Abschalten wird bei ExpertSDR3 bis zur Bestätigung wiederholt.
- **SWR-Prüfung:** Nach der Abstimmung wird das SWR über die TX-Sensoren von ExpertSDR3 gemessen und gegen einen Grenzwert geprüft.
- **Weboberfläche:** Status, die letzten 10 Abstimmungen mit Frequenz, Ergebnis, SWR und Dauer, dazu alle Einstellungen. Optional mit Passwortschutz.
- **Einrichtung ohne Programmierung:** Ohne WLAN startet ein Access-Point mit Konfigurationsseite (Captive Portal).
- **Status-LED:** Zeigt WLAN, TCI-Verbindung, laufende Abstimmung und Fehler.
- **Firmware-Update über die Weboberfläche:** mit automatischer Rückkehr zur alten Firmware, falls die neue nicht startet.
- **Erreichbar per mDNS:** unter `http://tci-tuner.local/`.

## Voraussetzungen

- ESP32-Board mit 4 MB Flash (z.B. ESP32-DevKitC, PlatformIO-Board `esp32dev`)
- SDR mit ExpertSDR3 und eingeschaltetem TCI-Server (*Options → TCI*)
- Tuner mit ICOM-AH-4-Schnittstelle
- Interface-Schaltung (siehe [Hardware](#hardware)) und 13,8-V-Versorgung
- [PlatformIO](https://platformio.org/) zum Bauen und Flashen; es verwendet ESP-IDF 5.4

## Ablauf einer Abstimmung

```
ExpertSDR3            Interface (ESP32)                  Tuner
TUNE:0,true;  ──────▶ START aktiv ─────────────────────▶ Reset, nach ca. 300 ms bereit
                      KEY aktiv              ◀─────────── KEY (stimmt ab, typ. 1–3 s)
                      START frei (250 ms nach KEY)
                      KEY frei               ◀─────────── fertig
                        KEY < 100 ms danach wieder aktiv ◀─ Fehlschlag (20 ms Lücke)
                      SWR messen (300 ms, TX_SENSORS)
TUNE:0,false; ◀────── Stopp, bis ExpertSDR3 bestätigt
```

Der Tune-Träger ist schon an, sobald TUNE gedrückt wird. Der AH-4 prüft während des Abstimmens, ob die Leistung zwischen 5 und 15 W liegt, und bricht sonst ab. Stell die Tune-Leistung in ExpertSDR3 deshalb auf etwa 10 W ein. Für andere Tuner gelten die Grenzen aus deren Anleitung.

| Ergebnis | Bedeutung |
|---|---|
| OK | Tuner fertig, SWR unter der Grenze (oder keine Prüfung) |
| SWR zu hoch | Tuner fertig, gemessenes SWR über „Max. SWR“ |
| Tuner meldet Fehlschlag | Tuner hat KEY nach der Freigabe kurz erneut gesetzt (keine Abstimmung gefunden) |
| Tuner antwortet nicht | KEY wurde nach START nicht innerhalb der „Wartezeit auf KEY“ aktiv |
| Timeout | Tuner nicht innerhalb des „Tune-Timeouts“ fertig |
| Abgebrochen | TUNE in ExpertSDR3 beendet oder TCI-Verbindung verloren |
| Stopp nicht bestätigt | ExpertSDR3 hat `TUNE:false` nach 6 Versuchen nicht bestätigt |

## Hardware

### AH-4-Schnittstelle

Das Steuerkabel hat vier Leitungen: +13,8 V, GND, START und KEY. START und KEY sind Open-Collector-Leitungen mit 12-V-Logik; aktiv heisst, die Leitung ist auf GND gezogen.

Die Pull-ups auf 13,8 V sitzen beim Original **im Funkgerät**. Das Interface ersetzt das Funkgerät und stellt sie deshalb selbst bereit. Der AH-4 zieht KEY intern zusätzlich über 22 kΩ und eine Diode auf 5 V. Der Tuner braucht typisch weniger als 300 mA, in der Spitze weniger als 1 A.

Kein ESP32-Pin ist direkt mit dem Tuner verbunden; beide Steuerleitungen laufen über einen Transistor.

```
Versorgung
  +13,8 V ──[Sicherung 1 A]──┬──────────────────────────── +13,8 V zum Tuner
                             └──[Step-down 5 V]─── 5V/VIN ESP32
  GND ───────────────────────────────────────────── GND Tuner und ESP32 (gemeinsam)

START (GPIO27 → Tuner), GPIO27 HIGH = START aktiv
                                    +13,8 V
                                       │
                                     [4k7]  R2 (Pull-up, ersetzt den des Funkgeräts)
                                       │
  GPIO27 ──[4k7]──┬── B  Q1        C ──┴──────┬──── START
             R1   │      BC547                ═ C1 10 nF
                [100k] R3        E            │
                  │              │            │
  GND ────────────┴──────────────┴────────────┴──── GND

KEY (Tuner → GPIO26), GPIO26 HIGH = KEY aktiv
            +13,8 V                          +3,3 V
               │                                │
             [10k]  R4 (Pull-up)              [10k] R7 (nur auf 3,3 V!)
               │                                │
  KEY ─────────┼──┬──[47k]──┬── B  Q2      C ───┴──── GPIO26
                  ═ C2  R5  │      BC547
                  │ 10 nF [10k] R6       E
                  │         │            │
  GND ────────────┴─────────┴────────────┴─────────── GND
```

| Zustand | START-Leitung | KEY-Leitung | GPIO26 |
|---|---|---|---|
| Ruhe | ca. 13,8 V (R2) | ca. 11 V (R4) | LOW (Q2 leitet) |
| Interface startet Abstimmung | GND (Q1 leitet) | | |
| Tuner stimmt ab | | GND (Tuner) | HIGH (Q2 sperrt) |

- R3 hält Q1 gesperrt, solange GPIO27 beim Booten hochohmig ist. So entsteht kein ungewollter START.
- R6 sperrt Q2 sicher, wenn der Tuner KEY auf GND zieht (Restspannung des Open-Collectors).
- C1 und C2 gehören direkt an die Anschlussbuchse; sie halten HF von den Steuerleitungen fern.
- Bei dieser Schaltung ist KEY am ESP32 HIGH-aktiv, das ist die Voreinstellung. Ohne invertierenden Transistor im KEY-Pfad in der Weboberfläche „LOW“ wählen.
- Belegung und Aderfarben des Steuerkabels stehen in der Anleitung des Tuners.

| Signal | GPIO | Pegel am ESP32 |
|---|---|---|
| Status-LED | 2 | HIGH = an (Onboard-LED des DevKits) |
| START → Tuner | 27 | HIGH = aktiv |
| KEY ← Tuner | 26 | HIGH = aktiv (einstellbar) |

Die Pin-Zuordnung steht in [src/hw_config.h](src/hw_config.h).

### Test ohne Tuner

- Schliess einen Taster zwischen KEY und GND an.
- **Erfolg:** TUNE in ExpertSDR3 drücken, innerhalb von 2 s den Taster drücken (der Tuner „stimmt ab“) und wieder loslassen (fertig). Ergebnis: „OK“.
- **Fehlschlag:** Nach dem Loslassen innerhalb von 100 ms kurz nochmals drücken. Ergebnis: „Tuner meldet Fehlschlag“.
- **START sichtbar machen:** Eine LED mit Vorwiderstand von +13,8 V auf START leuchtet, solange START aktiv ist.

## Installation

```
git clone <repository-url>
cd <repository>
pio run -t upload
pio device monitor
```

Beim ersten Build lädt PlatformIO ESP-IDF und die Komponenten aus [src/idf_component.yml](src/idf_component.yml) herunter (`esp_websocket_client`, `mdns`). Die genauen Versionen stehen in [dependencies.lock](dependencies.lock).

### Update über die Weboberfläche

Ist die Firmware einmal per USB installiert, geht jedes weitere Update auch über das WLAN:

1. `pio run` ausführen. Die Datei `.pio/build/esp32dev/firmware.bin` entsteht.
2. Auf der Weboberfläche unter **Firmware-Update** die Datei wählen und **Installieren** klicken.
3. Das Interface schreibt die Firmware in die freie App-Partition und startet neu. Die Einstellungen bleiben erhalten.

Das Interface prüft vor dem Schreiben, ob die Datei eine Firmware dieses Projekts ist. Während einer Abstimmung ist kein Update möglich. Die neue Firmware gilt erst als gültig, wenn sie beim Start den Webserver erreicht. Stürzt sie vorher ab, kehrt der Bootloader beim nächsten Neustart zur bisherigen Firmware zurück (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`).

Wer per curl aktualisieren will:

```
curl -H "Content-Type: application/octet-stream" --data-binary @.pio/build/esp32dev/firmware.bin http://tci-tuner.local/api/update
```

Mit Passwortschutz kommt `-u admin:<passwort>` dazu.

## Einrichtung

1. **Access-Point:** Nach dem ersten Start, oder wenn 30 s lang keine WLAN-Verbindung zustande kommt, öffnet das Interface den Access-Point `TCI-Tuner-XXXX` mit dem Passwort `tci-tuner`.
2. **Konfigurationsseite:** Nach dem Verbinden öffnet sich die Seite meist von selbst, sonst `http://192.168.4.1/` aufrufen. Wichtig ist das `http://`: Bei einer zusätzlichen LAN-Verbindung oder mit „Sicherem DNS“ im Browser landet die automatische Umleitung von Windows sonst im Internet.
3. **Eintragen:** WLAN, dazu TCI-Host (IP des PCs mit ExpertSDR3) und Port, dann speichern. Das Interface startet neu.
4. **Betrieb:** Danach ist das Interface unter `http://tci-tuner.local/` oder seiner IP-Adresse erreichbar.

### Einstellungen

| Einstellung | Standard | Beschreibung |
|---|---|---|
| WLAN-SSID / Passwort | – | 2,4-GHz-WLAN; Passwortfeld leer lassen = unverändert |
| Hostname | `tci-tuner` | Erreichbar als `<hostname>.local` |
| TCI-Host / Port | – / 40001 | Adresse des TCI-Servers von ExpertSDR3 |
| Transceiver | alle | Auf TUNE welches Transceivers reagiert wird |
| KEY-Eingang aktiv bei | HIGH | Pegel an GPIO26, wenn der Tuner KEY aktiviert |
| START halten | 250 ms | So lange bleibt START nach Aktivierung von KEY aktiv |
| Wartezeit auf KEY | 2000 ms | Ab Start; danach Ergebnis „Tuner antwortet nicht“ |
| Tune-Timeout | 20000 ms | Ab Start; danach wird der Träger abgeschaltet |
| SWR-Messzeit | 300 ms | Träger nach der Abstimmung so lange halten, um das SWR zu messen (0 = keine Messung) |
| Max. SWR | 2.0 | Grenzwert für die SWR-Prüfung (0 = keine Prüfung) |
| Web-Passwort | – | Schützt die Weboberfläche (Benutzer `admin`) |

Die Tuner-Einstellungen wirken sofort. Änderungen an WLAN oder Hostname führen zu einem Neustart.

### JSON-API

| Methode | Pfad | Inhalt |
|---|---|---|
| GET | `/api/status` | WLAN, TCI, Tuner-Zustand und Verlauf |
| GET | `/api/settings` | Einstellungen (ohne Passwörter) |
| POST | `/api/settings` | Einstellungen ändern (`application/x-www-form-urlencoded`) |
| POST | `/api/reboot` | Neustart |
| POST | `/api/update` | Firmware-Update, Body ist die `firmware.bin` (`application/octet-stream`) |

## Status-LED

| Muster | Bedeutung |
|---|---|
| dauernd an | TCI verbunden und bereit |
| langsam blinkend | WLAN verbunden, TCI nicht bereit |
| Doppelblitz | Konfigurations-Access-Point aktiv |
| kurzer Blitz alle 2 s | kein WLAN |
| schnell blinkend | Abstimmung läuft |
| Dreifachblitz | letzte Abstimmung fehlgeschlagen (30 s lang) |

## Hinweise

**WPA3:** WPA3 (SAE) ist in [sdkconfig.defaults](sdkconfig.defaults) abgeschaltet, weil der SAE-Handshake des ESP32 mit manchen Routern scheitert (`AUTH_EXPIRE`). An WPA2/WPA3-Routern meldet sich das Interface deshalb per WPA2 an; reine WPA3-Netze werden nicht unterstützt. Scheitert die WLAN-Verbindung, steht im seriellen Log ein Scan mit Kanal, Signalstärke und Verschlüsselung des Netzes.

**WebSocket-Puffer:** ExpertSDR3 sendet direkt nach dem Verbindungsaufbau einen grossen Block mit Initialisierungsdaten. Dafür ist der Puffer des WebSocket-Transports auf 8 KB vergrössert (`CONFIG_WS_BUFFER_SIZE`).

## Projektstruktur

| Datei | Inhalt |
|---|---|
| `src/main.cpp` | Initialisierung, Hauptschleife (5 ms), Wahl des LED-Musters |
| `src/tci_client.*` | TCI-Client (`esp_websocket_client`), Auswertung von `tune`, `tx_sensors`, `vfo`, `ready` |
| `src/tuner.*` | AH-4-Zustandsmaschine mit Timeouts, Fehlschlag-Erkennung, SWR-Prüfung, Verlauf |
| `src/network.*` | WLAN, Access-Point-Fallback, Captive Portal (DHCP-Option 114), mDNS |
| `src/dns_server.*` | DNS-Server für das Captive Portal |
| `src/web_ui.*`, `src/index.html` | Weboberfläche, JSON-API und Firmware-Update (`esp_http_server`, `app_update`) |
| `src/settings.*` | Einstellungen im NVS |
| `src/hw_config.h` | Pin-Zuordnung |
| `sdkconfig.defaults` | ESP-IDF-Konfiguration |
| `partitions.csv` | Partitionstabelle mit zwei App-Partitionen für Updates über die Weboberfläche |

Die Anwendungslogik läuft in einer einzigen Hauptschleife. Der WebSocket-Client übergibt seine Ereignisse über eine Queue, die HTTP-Handler sperren einen gemeinsamen Mutex (`appMutex()`).

## Quellen

- [ExpertSDR3 TCI-Protokoll](https://github.com/ExpertSDR3/TCI)
- K9EQ: [Inside the Icom AH-4 Tuner](https://www.hamoperator.com/HF/AH-4_Design_and_Operation.pdf) (Ablauf, Pegel, Fehlschlag-Signal)
- K9EQ: [AH-4 Universal Interface](https://www.hamoperator.com/Hamoperator/AH-4_Universal_Interface_files/ah4-manual-5.pdf)

## Copyright

© 2026 HB9DUT
