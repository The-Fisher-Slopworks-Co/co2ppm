// SPDX-FileCopyrightText: 2026 The Fisher Slopworks Co
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <stdbool.h>
#include <stddef.h>

// WiFi station with web-based provisioning and an automatic setup fallback.
//
// Credentials live in NVS, not in the firmware image, so the network can be
// changed without reflashing. The behaviour:
//
//   * Boot with stored credentials -> connect as a station, auto-reconnect on
//     drop (exactly as before).
//   * Stay disconnected for longer than CONFIG_WIFI_SETUP_GRACE_SECONDS (and on
//     a fresh device with no credentials) -> bring up a SoftAP + captive-portal
//     setup page WHILE still retrying the saved network in the background.
//   * The saved network comes back (a transient outage, or freshly entered
//     credentials that work) -> the setup AP is torn down automatically.
//
// That last point is the whole point: a one-off failure of the correct network
// never leaves the device stranded in setup mode.
//
// Requires nvs_flash_init(), esp_netif_init() and
// esp_event_loop_create_default() to have been called first. Non-blocking:
// connection progress is driven by WiFi events, so the caller can start the
// HTTP server (which serves both /metrics and the setup page) right after.
void wifi_begin(void);

// True once the station has an IP, false while disconnected/reconnecting.
// Tracks the connection live, so it flips back to false if the link drops.
bool wifi_is_connected(void);

// True while the setup SoftAP + captive portal is active.
bool wifi_in_setup(void);

// Persist new credentials to NVS and (re)connect with them immediately. Works
// both from the setup portal and from the always-available page on the running
// station, so the network can be switched even while currently connected. If
// the new credentials fail, the usual grace-period fallback takes over.
void wifi_apply_credentials(const char *ssid, const char *password);

// Copy the currently configured station SSID into out (NUL-terminated, empty if
// none). Used to pre-fill the setup form.
void wifi_get_ssid(char *out, size_t len);

// Max bytes for an SSID buffer (32-char SSID + NUL).
#define WIFI_SSID_MAXLEN 33

// Scan for nearby networks, writing up to `max` unique, non-hidden SSIDs into
// `ssids`. Returns the count found (0 on failure). Safe to call in setup mode:
// it briefly pauses the reconnect loop so an in-flight connection attempt can't
// block the scan, then resumes it. Blocks for the duration of the scan (~2 s).
int wifi_scan_ssids(char ssids[][WIFI_SSID_MAXLEN], int max);

// Erase the stored credentials from NVS (a factory reset). Takes effect on the
// next boot, which then comes up with no credentials and opens the setup portal.
// Does not touch the live connection; pair it with esp_restart().
void wifi_clear_credentials(void);
