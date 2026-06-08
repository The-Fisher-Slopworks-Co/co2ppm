// SPDX-FileCopyrightText: 2026 The Fisher Slopworks Co
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// Connects to WiFi as a station using the hardcoded credentials from
// menuconfig (CONFIG_WIFI_SSID / CONFIG_WIFI_PASSWORD). Auto-reconnects on
// disconnect. Blocks for up to ~30 s waiting for the first IP, then returns
// regardless so the rest of boot can proceed.
//
// Requires nvs_flash_init(), esp_netif_init() and
// esp_event_loop_create_default() to have been called first.
void wifi_begin(void);
