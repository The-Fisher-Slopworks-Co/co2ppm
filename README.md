<!--
SPDX-FileCopyrightText: 2026 The Fisher Slopworks Co
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# CO2 Exporter (ESP32 / ESP-IDF)

ESP32 firmware that reads CO2 and temperature data from a ZyAura ZG-01 sensor
(Voltcraft CO-100 and compatible) and exposes it as
[Prometheus](https://prometheus.io/) metrics over HTTP.

This is an ESP-IDF port of the original ESP8266/Arduino project. The WiFi
provisioning captive portal has been removed — credentials are hardcoded at
build time (see [Configuration](#configuration)).

## Features

- Reads CO2 (ppm) and temperature (°C) from ZG-01 via interrupt-driven bit-banging
- HTTP `/metrics` endpoint in Prometheus text exposition format 0.0.4
- WiFi station with hardcoded credentials, configurable hostname, and automatic reconnect

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
| `WIFI_SSID` | `YOUR_WIFI_SSID` | Network to connect to (**must change**) |
| `WIFI_PASSWORD` | `YOUR_WIFI_PASSWORD` | WiFi password (**must change**) |
| `HOSTNAME` | `co2meter` | Hostname advertised to the network (DHCP/mDNS) |
| `DEVICE_NAME` | `daget` | `device="..."` label on every metric |
| `PIN_CLK` | `5` | ZG-01 clock GPIO |
| `PIN_DATA` | `4` | ZG-01 data GPIO |

You can also set the WiFi credentials in `sdkconfig.defaults` before the first
build:

```
CONFIG_WIFI_SSID="my-network"
CONFIG_WIFI_PASSWORD="my-password"
```

## Build & Flash

```bash
# one-time, per checkout
. $IDF_PATH/export.sh
idf.py set-target esp32

# configure WiFi credentials and any other settings
idf.py menuconfig

# build, flash, and watch the serial log
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

For other ESP32 variants use the matching target, e.g.
`idf.py set-target esp32c3`.

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
