# TCI to ICOM Tuner Interface

ESP32 firmware that connects an automatic antenna tuner with an **ICOM AH-4 interface** to an SDR running **TCI-Protocol** like ExpertSDR. When you press TUNE in SDR Software, the interface starts the tuner, waits for tuning to finish, checks the SWR and switches the tune carrier off again.

Towards the tuner, the interface takes the place of an ICOM radio. This makes it suitable for the ICOM AH-4 and compatible tuners such as the Stockcorner, GC-3000 or others. The link to SDR-Software uses the [TCI protocol](https://github.com/ExpertSDR3/TCI) (WebSocket) over Wi-Fi, so no cables to the SDR are needed.

## Features

- **Triggered by TUNE in SDR-Software:** Tuning starts automatically, and the connection re-establishes itself within 5 s after an interruption, e.g. when the SDR software is restarted.
- **AH-4 sequence:** Holds START until the tuner asserts KEY, then detects the end of tuning as well as the tuner's failure signal.
- **Safety:** The tune carrier is switched off after an adjustable timeout at the latest, even if the tuner does not respond. The stop command is repeated until ExpertSDR3 confirms it.
- **SWR check:** After tuning, the SWR is measured via the TX sensors of ExpertSDR3 and checked against a limit.
- **Web interface:** Status, the last 10 tuning runs with frequency, result, SWR and duration, and all settings. Optional password protection.
- **Tune button in the web interface:** Starts a tune from the browser (the interface sets TUNE via TCI itself) and can stop it again.
- **Setup without programming:** Without Wi-Fi, the interface opens an access point with a configuration page (captive portal). Available Wi-Fi networks can be selected from a list.
- **Status LED:** Shows Wi-Fi, TCI connection, tuning in progress and errors.
- **Firmware update via the web interface:** Automatically falls back to the previous firmware if the new one does not start.
- **Reachable via mDNS:** at `http://tci-tuner.local/`.

The web interface is available in English and German and follows the browser language; it can be switched at the top right.

## Requirements

- ESP32 board with 4 MB flash (e.g. ESP32-DevKitC, PlatformIO board `esp32dev`)
- SDR with ExpertSDR3 and the TCI server enabled (*Options → TCI*)
- Tuner with an ICOM AH-4 interface
- Interface circuit (see [Hardware](#hardware)) and a 13.8 V supply
- Chrome or Edge for the [web installer](https://hb9dut.github.io/TCI_ICOM_Tuner_Interface/), or [PlatformIO](https://platformio.org/) to build from source (ESP-IDF 5.4)

## Tuning sequence

```
ExpertSDR3            Interface (ESP32)                  Tuner
TUNE:0,true;  ──────▶ START active ────────────────────▶ reset, ready after approx. 300 ms
                      KEY active             ◀─────────── KEY (tuning, typ. 1–3 s)
                      START released (250 ms after KEY)
                      KEY released           ◀─────────── done
                        KEY active again < 100 ms later ◀─ failure (20 ms gap)
                      measure SWR (300 ms, TX_SENSORS)
TUNE:0,false; ◀────── stop, repeated until ExpertSDR3 confirms
```

The tune carrier is already on as soon as TUNE is pressed. While tuning, the AH-4 checks that the power is between 5 and 15 W and aborts otherwise. Set the tune power in SDR-Software to about 10 W. For other tuners, use the limits from their manual.

| Result | Meaning |
|---|---|
| OK | Tuner done, SWR below the limit (or no check) |
| SWR too high | Tuner done, measured SWR above "Max. SWR" |
| Tuner reports failure | Tuner briefly asserted KEY again after releasing it (no match found) |
| Tuner not responding | KEY did not become active within the "KEY wait time" after START |
| Timeout | Tuner not done within the "Tune timeout" |
| Aborted | TUNE ended in ExpertSDR3 or TCI connection lost |
| Stop not confirmed | ExpertSDR3 did not confirm `TUNE:false` after 6 attempts |

## Hardware

### AH-4 interface

The control cable has four wires: +13.8 V, GND, START and KEY. START and KEY are open-collector lines with 12 V logic; active means the line is pulled to GND.

On the original setup, the pull-ups to 13.8 V are **inside the radio**. The interface replaces the radio and therefore provides them itself. The AH-4 additionally pulls KEY to 5 V internally via 22 kΩ and a diode. The tuner typically draws less than 300 mA, with peaks below 1 A.

No ESP32 pin connects directly to the tuner; both control lines go through a transistor.

![Schematic: AH-4 interface supply, START and KEY circuits](ah4_interface_schematic.svg)

| State | START line | KEY line | GPIO26 |
|---|---|---|---|
| Idle | approx. 13.8 V (R2) | approx. 11 V (R4) | LOW (Q2 conducts) |
| Interface starts tuning | GND (Q1 conducts) | | |
| Tuner is tuning | | GND (tuner) | HIGH (Q2 off) |

- With this circuit, KEY is active HIGH at the ESP32, which is the default. Without an inverting transistor in the KEY path, select "LOW" in the web interface.
- The pinout and wire colours of the control cable are in the tuner's manual.

| Signal | GPIO | Level at the ESP32 |
|---|---|---|
| Status LED | 2 | HIGH = on (DevKit onboard LED) |
| START → tuner | 27 | HIGH = active |
| KEY ← tuner | 26 | HIGH = active (configurable) |

The pin assignment is in [src/hw_config.h](src/hw_config.h).

### Testing without a tuner

- Connect a push button between KEY and GND.
- **Success:** Press TUNE in ExpertSDR3, press the button within 2 s (the tuner is "tuning") and release it (done). Result: "OK".
- **Failure:** After releasing, briefly press again within 100 ms. Result: "Tuner reports failure".
- **Making START visible:** An LED with a series resistor from +13.8 V to START lights up while START is active.

## Installation

Open the **[web installer](https://hb9dut.github.io/TCI_ICOM_Tuner_Interface/)** in Chrome or Edge, connect the ESP32 via USB and click *Install firmware*. No software needs to be installed.

The firmware runs on boards with the classic ESP32 chip and at least 4 MB flash; ESP32-S2, -S3, -C3 and -C6 are not supported.

Alternatively, every [release](https://github.com/HB9DUT/TCI_ICOM_Tuner_Interface/releases/latest) contains `tci-tuner-<version>-full.bin`, which can be written with esptool at address `0x0`:

```
esptool.py --chip esp32 write_flash 0x0 tci-tuner-<version>-full.bin
```

### Updates

Once installed, updates are done in the web interface of the device:

1. Download `firmware.bin` from the [latest release](https://github.com/HB9DUT/TCI_ICOM_Tuner_Interface/releases/latest).
2. In the web interface, under **Firmware update**, select the file and click **Install**.
3. The interface writes the firmware to the free app partition and restarts. Settings are kept.

Before writing, the interface checks that the file is firmware for this project. Updates are blocked while tuning is in progress. The new firmware is only marked valid once it reaches the web server at startup. If it crashes before that, the bootloader falls back to the previous firmware on the next restart (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`).

The web installer can also be used for updates. Settings are kept unless you choose to erase the device.

## Setup

1. **Access point:** On first start, or if no Wi-Fi connection is established for 30 s, the interface opens the access point `TCI-Tuner-XXXX` with the password `tci-tuner`.
2. **Configuration page:** After connecting, the page usually opens by itself; otherwise open `http://192.168.4.1/`. The `http://` matters: with an additional LAN connection or "Secure DNS" enabled in the browser, the automatic redirect in Windows otherwise ends up on the internet.
3. **Enter settings:** Select the Wi-Fi network from the list (or click *Scan*), enter its password, plus TCI host (IP of the PC running SDR-Software) and port, then save. The interface restarts.
4. **Operation:** The interface is then reachable at `http://tci-tuner.local/` or its IP address.

### Settings

| Setting | Default | Description |
|---|---|---|
| Wi-Fi SSID / password | – | 2.4 GHz Wi-Fi; leave the password field empty to keep it unchanged |
| Hostname | `tci-tuner` | Reachable as `<hostname>.local` |
| TCI host / port | – / 40001 | Address of the SDR TCI server |
| Transceiver | all | Which transceiver's TUNE to respond to |
| KEY input active on | HIGH | Level at GPIO26 when the tuner asserts KEY |
| Hold START | 250 ms | How long START stays active after KEY is asserted |
| KEY wait time | 2000 ms | From start; then result "Tuner not responding" |
| Tune timeout | 20000 ms | From start; then the carrier is switched off |
| SWR measuring time | 300 ms | Keep the carrier on this long after tuning to measure the SWR (0 = no measurement) |
| Max. SWR | 2.0 | Limit for the SWR check (0 = no check) |
| Web interface password | – | Protects the web interface (user `admin`) |

Tuner settings take effect immediately. Changing Wi-Fi or hostname triggers a restart.

### JSON API

| Method | Path | Content |
|---|---|---|
| GET | `/api/status` | Wi-Fi, TCI, tuner state and history |
| GET | `/api/settings` | Settings (without passwords) |
| POST | `/api/settings` | Change settings (`application/x-www-form-urlencoded`) |
| POST | `/api/reboot` | Restart |
| POST | `/api/tune` | Start (`on=1`) or stop (`on=0`) a tune |
| GET | `/api/scan` | Visible Wi-Fi networks (takes about 3 s, blocked while tuning) |
| POST | `/api/update` | Firmware update, body is the `firmware.bin` (`application/octet-stream`) |

### SDR software

| Software | TUNE in the SDR starts the tuner | Interface can stop the tune | Tune button |
|---|---|---|---|
| ExpertSDR3 | yes | yes | yes |
| Thetis | yes | yes | yes |
| deskHPSDR | yes | only if the tune was started via TCI | yes |
| AetherSDR | no (tune changes are not reported to TCI clients) | yes | yes |

deskHPSDR ignores a TCI stop request for a tune that was started in deskHPSDR itself. With the tune button in the web interface, the interface starts the tune and can stop it again.

## Status LED

| Pattern | Meaning |
|---|---|
| steady on | TCI connected and ready |
| slow blinking | Wi-Fi connected, TCI not ready |
| double flash | configuration access point active |
| short flash every 2 s | no Wi-Fi |
| fast blinking | tuning in progress |
| triple flash | last tuning failed (for 30 s) |

## Notes

**WPA3:** WPA3 (SAE) is disabled in [sdkconfig.defaults](sdkconfig.defaults) because the ESP32's SAE handshake fails with some routers (`AUTH_EXPIRE`). On WPA2/WPA3 routers the interface therefore connects using WPA2; WPA3-only networks are not supported. If the Wi-Fi connection fails, the serial log shows a scan with the network's channel, signal strength and encryption.

## Building from source

```
git clone https://github.com/HB9DUT/TCI_ICOM_Tuner_Interface.git
cd TCI_ICOM_Tuner_Interface
pio run -t upload
pio device monitor
```

On the first build, PlatformIO downloads ESP-IDF and the components listed in [src/idf_component.yml](src/idf_component.yml) (`esp_websocket_client`, `mdns`). The exact versions are in [dependencies.lock](dependencies.lock).

The version number comes from the Git tag (`git describe`). Builds between releases show e.g. `2.4.0-3-gabc1234`; after a new commit, run `pio run -t clean` so the number is updated.

A self-built `.pio/build/esp32dev/firmware.bin` can be installed via the web interface or with curl:

```
curl -H "Content-Type: application/octet-stream" --data-binary @.pio/build/esp32dev/firmware.bin http://tci-tuner.local/api/update
```

With password protection, add `-u admin:<password>`.

## References

- [ExpertSDR3 TCI protocol](https://github.com/ExpertSDR3/TCI)
- K9EQ: [Inside the Icom AH-4 Tuner](https://www.hamoperator.com/HF/AH-4_Design_and_Operation.pdf) (sequence, levels, failure signal)
- K9EQ: [AH-4 Universal Interface](https://www.hamoperator.com/Hamoperator/AH-4_Universal_Interface_files/ah4-manual-5.pdf)

## Disclaimer

This project is provided "as is", without warranty of any kind. Building and using it is entirely at your own risk.

- **You are responsible for your station.** The interface keys a transmitter via ExpertSDR3. Make sure your setup, tune power and antenna are suitable, and that you operate within the terms of your amateur radio licence and local regulations.
- **Check the hardware yourself.** The interface circuit is connected to a 13.8 V supply, a tuner and RF equipment. Wiring errors can damage the ESP32, the tuner, the radio or the power supply. Verify the circuit, the pinout of your tuner and all levels before connecting anything.
- **No guarantee of correct function.** Timeouts and checks reduce the risk of an unattended carrier or a bad match, but they cannot rule out software or hardware faults. Do not leave the station unattended while tuning.
- The author accepts no liability for damage to equipment, injury, interference or any other consequences arising from the use of this project.

ICOM and AH-4 are trademarks of Icom Inc.; ExpertSDR3 and SunSDR are trademarks of Expert Electronics. This project is not affiliated with or endorsed by these companies.

## License

© 2026 HB9DUT

This program is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version (`GPL-3.0-or-later`).

This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the [LICENSE](LICENSE) file for details.
