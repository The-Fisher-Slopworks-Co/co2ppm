# SPDX-FileCopyrightText: 2026 The Fisher Slopworks Co
# SPDX-License-Identifier: AGPL-3.0-or-later

idf_component_register(
    SRCS
        "main.c"
        "zyaura.c"
        "wifi.c"
        "metrics_server.c"
    INCLUDE_DIRS "."
    REQUIRES
        nvs_flash
        esp_wifi
        esp_event
        esp_netif
        esp_http_server
        esp_timer
        esp_driver_gpio
        lwip
)
