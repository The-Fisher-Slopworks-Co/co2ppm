<!--
SPDX-FileCopyrightText: 2026 The Fisher Slopworks Co
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# CO2 Exporter (ESP32 / ESP-IDF)

ESP32 firmware that reads CO2 and temperature data from a ZyAura ZG-01 sensor
(Voltcraft CO-100 and compatible) and exposes it as
[Prometheus](https://prometheus.io/) metrics over HTTP.

This is an ESP-IDF port of the original ESP8266/Arduino project. WiFi
credentials are entered through a web setup portal and stored in NVS — no
reflashing to change networks (see [WiFi setup](#wifi-setup)).

## Features

- Reads CO2 (ppm) and temperature (°C) from ZG-01 via interrupt-driven bit-banging
- HTTP `/metrics` endpoint in Prometheus text exposition format 0.0.4
- Web-based WiFi setup (captive portal) with credentials stored in NVS, and a
  self-healing fallback that never gets stuck in setup after a transient outage
  (see [WiFi setup](#wifi-setup))
- Long-press factory reset on the on-board BOOT button (see [Factory reset](#factory-reset))
- On-board status LED that signals boot and runtime state at a glance (see [Status LED](#status-led))

## Hardware

| Component | Description |
|-----------|-------------|
| ESP32 dev board | Main microcontroller |
| ZyAura ZG-01 | CO2 + temperature sensor (e.g. Voltcraft CO-100) |

### Wiring (default pins)

```
GPIO5  → ZG-C (clock)
GPIO4  → ZG-D (data)
GND    → ZG-G
GPIO2  → status LED (on-board on most DevKit boards)
```

Pins are configurable via `idf.py menuconfig` (see below).

> **Note:** Some ZG-01 units have the connector in `D, C, G` order instead of
> `C, D, G`. If you get no readings, try swapping CLK and DATA.
>
> The default clock pin **GPIO5** is a strapping pin, but is safe here since it
> is only ever read as an input after boot.

> Sensor protocol reference: https://revspace.nl/CO2MeterHacking

## Requirements

[ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/)
**v5.x or v6.x** (developed and built against v6.0).

## Configuration

All settings live in `idf.py menuconfig` under **CO2 Exporter Configuration**
(stored in `sdkconfig`). The defaults come from `main/Kconfig.projbuild`.

| Setting | Default | Description |
|---------|---------|-------------|
| `WIFI_SETUP_GRACE_SECONDS` | `30` | Seconds offline before the setup portal opens (see [WiFi setup](#wifi-setup)) |
| `HOSTNAME` | `co2meter` | Hostname (DHCP/mDNS); also the base for the setup AP name |
| `DEVICE_NAME` | `daget` | `device="..."` label on every metric |
| `PIN_CLK` | `5` | ZG-01 clock GPIO |
| `PIN_DATA` | `4` | ZG-01 data GPIO |
| `STATUS_LED_ENABLE` | `y` | Drive an on-board status LED |
| `PIN_STATUS_LED` | `2` | Status LED GPIO |
| `STATUS_LED_ACTIVE_LOW` | `n` | Set if the LED lights when the pin is driven low |
| `RESET_BUTTON_ENABLE` | `y` | Long-press factory reset button (see [Factory reset](#factory-reset)) |
| `PIN_RESET_BUTTON` | `0` | Reset button GPIO (GPIO0 = on-board BOOT button) |
| `RESET_HOLD_SECONDS` | `3` | Seconds to hold the button before it triggers |

WiFi credentials are **not** configured here — they are entered at runtime
through the [setup portal](#wifi-setup) and stored in NVS.

## Build & Flash

```bash
# one-time, per checkout
. $IDF_PATH/export.sh
idf.py set-target esp32

# optional: adjust pins, hostname, or the setup grace period
# (WiFi credentials are NOT set here — see "WiFi setup" below)
idf.py menuconfig

# build, flash, and watch the serial log
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

For other ESP32 variants use the matching target, e.g.
`idf.py set-target esp32c3`.

## WiFi setup

Credentials are stored in NVS and entered over the air — there is nothing to
hardcode and no reflashing when your network changes.

**First boot (or no stored credentials):** the device opens an open WiFi network
named `<hostname>-setup` (e.g. `co2meter-setup`). Connect to it from a phone or
laptop; a captive-portal page should pop up automatically (if not, browse to
`http://192.168.4.1`). Pick your network from the list (or type the SSID),
enter the password, and save. The device connects and the setup network
disappears.

**Changing the network later:** while the device is connected, its setup page is
always available at `http://<device-ip>/setup` (also linked from the landing
page). Submit new credentials there at any time.

**Self-healing fallback:** if the saved network can't be reached, the device
keeps retrying it in the background *and* raises the setup portal after
`WIFI_SETUP_GRACE_SECONDS` (default 30 s) so you can reconfigure. Crucially, a
one-off outage of the *correct* network — a router reboot, a power blip — does
**not** strand the device in setup: as soon as the network returns it reconnects
on its own and tears the setup portal back down.

## Factory reset

To wipe the stored WiFi credentials, **press and hold the on-board BOOT button
(GPIO0) for 3 seconds** while the device is running. It erases the credentials
and reboots straight into the [setup portal](#wifi-setup).

> The button is read only at runtime, so this is independent of GPIO0's
> boot-strapping role — holding BOOT *during power-on* still enters the ESP32
> serial bootloader as usual; that is a separate thing.

The button GPIO, hold time, and the feature itself are configurable under
**Factory reset button** in `idf.py menuconfig`.

## Status LED

The on-board LED (GPIO2 by default) blinks a pattern that tells you where the
firmware is in its boot/run cycle, so you can diagnose a board without a serial
monitor:

| Pattern | State | Meaning |
|---------|-------|---------|
| Solid on | Booting | Early init (NVS, network stack, sensor) |
| Slow blink (1 Hz) | Setup | Setup portal is up, waiting for WiFi credentials |
| Fast blink (~5 Hz) | Connecting | Joining WiFi, or the link is down |
| Double blink, then pause | Online | WiFi up, waiting for the first sensor frame |
| Brief heartbeat flash | Running | WiFi up and sensor data is flowing — all good |

The LED reflects live state: if WiFi drops it returns to the fast blink, and if
the sensor stops reporting it falls back to the double blink. Disable it (or
change the pin / polarity) under **Status LED** in `idf.py menuconfig`.

## Metrics

The device starts an HTTP server on port 80 after boot.

**`GET /metrics`** returns Prometheus-formatted data:

```
# HELP co2_ppm CO2 concentration in parts per million
# TYPE co2_ppm gauge
co2_ppm{device="daget"} 623

# HELP temperature_celsius Temperature in degrees Celsius
# TYPE temperature_celsius gauge
temperature_celsius{device="daget"} 23.44
```

Returns HTTP 503 while no sensor data has been received yet.

### Example Prometheus config

```yaml
scrape_configs:
  - job_name: co2
    static_configs:
      - targets: ['<device-ip>:80']
```

## License

CO2 Exporter is licensed under AGPL-3.0-or-later. See [LICENSE](LICENSE). The
project is [REUSE](https://reuse.software/)-compliant.

## Contributing

Contribution guidelines, the Code of Conduct, and the security policy are shared
org-wide via
[The-Fisher-Slopworks-Co/.github](https://github.com/The-Fisher-Slopworks-Co/.github).
