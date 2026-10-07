# ESP32-S3 GNSS Time Server (Stratum 1 NTP Server)

<p>
  <img alt="License" src="https://img.shields.io/badge/license-Apache--2.0-blue.svg">
  <img alt="ESP-IDF" src="https://img.shields.io/badge/ESP--IDF-%E2%89%A55.1-red.svg">
  <img alt="Platform" src="https://img.shields.io/badge/platform-ESP32--S3-blueviolet.svg">
</p>

Turn an **ESP32-S3** into a **Stratum 1 NTP time server**: it takes UTC time from a GNSS
module, disciplines its system clock with the 1PPS signal, and serves time to your LAN over
WiFi with **sub-millisecond accuracy**.

<p align="center">
  <img src="docs/images/preview.jpg" alt="ESP32-S3 GNSS time server" width="70%" />
</p>

[中文](README.md) · **English**

---

## Why this project

- **Truly Stratum 1** — locked directly to GNSS satellite time, with no upstream NTP server.
- **Sub-millisecond accuracy** — disciplined by 1PPS instead of network sync alone; the clock
  is slewed smoothly and **never jumps**, so clients see no discontinuities.
- **Low cost** — just an ESP32-S3, a GNSS module and an OLED; no dedicated timing hardware.
- **Works out of the box** — automatic GNSS baud-rate detection, auto retry/restart on signal
  loss, and phone-based web provisioning (no re-flashing to change WiFi).
- **Transparent status** — the OLED shows fix status, time, sync residual, coordinates, IP and PPS state.

## Features

| Feature | Description |
| --- | --- |
| **GNSS timing** | Parses NMEA over UART for UTC time and position; works with common modules |
| **PPS discipline** | Hardware-captured 1PPS, corrected smoothly via `adjtime()` |
| **NTP service** | Standard port 123, IPv4 / IPv6, NTPv3 / NTPv4 |
| **Web provisioning** | Long-press to start an AP, configure WiFi from a phone browser, credentials persisted |
| **OLED display** | Two status pages, switched with a button |
| **Optional auth** | NTPv4 symmetric-key (SHA-256 MAC), disabled by default |

## How it works

```
GNSS module ──NMEA (UART)──▶ parse UTC ──first sync──▶ settimeofday()  one jump
     │
     └──1PPS──▶ hardware capture ──▶ per-second phase error ──▶ adjtime()  smooth slew
                                                                   │
                                             NTP clients ◀── NTP server (reply carries ref. timestamp)
```

- **Jump once**: after a fix, the system clock is set to GNSS time in a single step.
- **Slew forever**: on every PPS pulse the offset from the exact second is measured and removed
  with `adjtime()`, which gently speeds up or slows down the clock — **never a jump**.
- **Guard thresholds**: offsets below 2 µs are ignored; offsets above 250 ms are rejected as
  unreliable, preventing oscillation and mis-correction.

---

## Hardware & wiring

### Bill of materials

| Part | Notes |
| --- | --- |
| MCU | ESP32-S3 dev board |
| GNSS module | NMEA output with a PPS pin (e.g. u-blox M10 / NEO series) |
| Display | SSD1306 128×64 OLED (I2C) |
| Button | 1 tactile button (one leg to a GPIO, the other to GND) |

### Pinout

Pin definitions live in [`main/include/app_config.h`](main/include/app_config.h) and can be changed.

| ESP32-S3 pin | Connects to | Notes |
| --- | --- | --- |
| GPIO41 | GNSS RXD | ESP32-S3 TX |
| GPIO42 | GNSS TXD | ESP32-S3 RX |
| GPIO45 | GNSS PPS | 1PPS input |
| GPIO2 | OLED SCL | I2C clock |
| GPIO1 | OLED SDA | I2C data |
| GPIO21 | Button | Pulls to GND when pressed |
| GND | GNSS / OLED | Common ground |

---

## Quick start

1. Wire everything up as above.
2. Set your WiFi SSID and password in [`main/include/app_config.h`](main/include/app_config.h).
3. Build and flash (see below).
4. Power on and wait for WiFi and a GNSS fix (the OLED shows `SAT:xx FIX`).
5. Note the IP shown on the OLED and point your PC / router NTP client at it.

> The first fix can take a few minutes depending on antenna placement and sky view; outdoors or
> near a window works best.

## Build & flash

Requires [ESP-IDF](https://docs.espressif.com/projects/esp-idf/) **v5.1 or newer**
(adapted for the mbedTLS 4.x / PSA Crypto changes in v6.1).

```bash
idf.py set-target esp32s3
idf.py build
idf.py -p COMx flash monitor   # replace with your port
```

---

## Configuration

Everything is configured in [`main/include/app_config.h`](main/include/app_config.h). **Required**:

```c
#define WIFI_SSID     "YOUR_WIFI_SSID"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"
```

Other common options:

| Option | Default | Description |
| --- | --- | --- |
| `TIME_ZONE_SPEC` | `"CST-8"` | Time zone (POSIX TZ). Display only — NTP always serves UTC |
| `GNSS_TX_PIN` / `GNSS_RX_PIN` | 41 / 42 | GNSS UART pins |
| `GNSS_PPS_PIN` | 45 | PPS input pin |
| `OLED_SCK_PIN` / `OLED_SDA_PIN` | 2 / 1 | OLED I2C pins |
| `BUTTON_PIN` | 21 | Button pin |
| `BUTTON_LONG_PRESS_MS` | 5000 | Long-press duration to enter provisioning |
| `PROVISION_AP_PASSWORD` | `"12345678"` | Provisioning AP password; leave empty for an open AP |
| `PERIODIC_GNSS_REFRESH_MINUTES` | 5 | Re-sync with GNSS every N minutes |
| `SYMMETRIC_KEY_AUTHENTICATION_ENABLED` | 0 | Enable NTPv4 symmetric-key authentication |
| `DEBUG_ENABLED` | 0 | Verbose serial logging |

> **Symmetric keys**: when enabled, fill in [`main/ntp/ntp_auth.c`](main/ntp/ntp_auth.c) →
> `symmetric_keys[]` with a key of **exactly 32 alphanumeric characters** and a unique, non-zero
> `key_id`; replace the placeholder value.

---

## Usage

### Boot sequence

Init display & storage → connect WiFi → wait for GNSS fix and stable PPS → first time sync →
start the NTP server.

### OLED pages (short press to switch)

**Status page**

| Line | Description |
| --- | --- |
| `SAT:xx FIX / NO FIX` | Satellite count and fix state (`FIX` = locked) |
| Date / time | Local time, affected by the time zone |
| `DELTA:...` | Residual of the last sync — smaller is better |

**Position page**

| Line | Description |
| --- | --- |
| `LAT` / `LON` | Latitude / longitude |
| `IP:...` | Device IPv4 address, used to configure NTP clients |
| `PPS:ACTIVE / LOST` | 1PPS state (`ACTIVE` = disciplining) |

During provisioning, `CONFIG MODE` and `WIFI SAVED` pages are shown instead.

### Button

- **Short press** — switch OLED page.
- **Long press (~5 s)** — start WiFi provisioning AP.

### WiFi provisioning

1. Long-press the button for ~5 s; the OLED shows `CONFIG MODE` (AP name, password, config URL).
2. Join the AP from your phone — the name looks like `ESPTIME-1A2B`.
3. Open **`http://192.168.4.1/`**.
4. Pick your WiFi, enter the password and save.
5. The AP closes; the device reconnects with the new credentials (OLED shows `WIFI SAVED`).

> Provisioned credentials are stored on the device and **take priority over** the compile-time
> values in `app_config.h`; they are reused on the next boot.

---

## Pointing NTP clients at it

Find the IP on the OLED position page (`IP:`); the port is the standard **123**.

**Windows** (admin PowerShell)

```powershell
w32tm /config /manualpeerlist:"<device-ip>,0x9" /syncfromflags:manual /update
w32tm /resync
```

**Linux (chrony)** — edit `/etc/chrony/chrony.conf`:

```
server <device-ip> iburst
```

**Linux (systemd-timesyncd)** — edit `/etc/systemd/timesyncd.conf`:

```
[Time]
NTP=<device-ip>
```

> With symmetric-key authentication enabled, the boot log prints ready-to-paste key lines for
> Meinberg (`ntp.keys`) and Chrony (`chrony.keys`).

---

## Troubleshooting

| Symptom | Likely cause / fix |
| --- | --- |
| `SAT:-- no data` | No GNSS data. Check TX/RX are not swapped, power, common ground and baud rate |
| `NO FIX` for a long time | Poor sky view — move outdoors or near a window and wait a few minutes; verify the antenna |
| `PPS:LOST` | PPS not connected or wrong level. Wire to 3.3 V, keep it short, share ground |
| `DELTA` looks large | Normal right after the first sync or a recovery; it converges to milliseconds |
| Can't connect to WiFi | Long-press to re-enter provisioning; password must be ≥ 8 chars (or empty) |
| No device IP | Not on WiFi yet — check the serial log and re-provision if needed |
| Boot loop | Usually GNSS never becoming ready. Check wiring, power and module compatibility; the log explains |

Debug tip: set `DEBUG_ENABLED` to `1` for more verbose logs (a small overhead under load).

---

## License & credits

- This project is released under the **Apache License 2.0** — see [LICENSE](LICENSE).
- It is a C port of [roblatour / ESP32TimeServer](https://github.com/roblatour/ESP32TimeServer);
  the portions ported from upstream remain under their original **MIT license**
  (Copyright (c) 2026 Rob Latour) — see [LICENSES/MIT.txt](LICENSES/MIT.txt).
- Full third-party attributions are in [NOTICE](NOTICE).

> This follows the compatible MIT → Apache-2.0 pattern: the project's own code is Apache-2.0 while
> the upstream MIT code keeps its original notice; both coexist in this repository.

## Author

- Author: yourlai
- Website: [yourlai.com](https://yourlai.com)
- Email: <yourlai@yourlai.icu>