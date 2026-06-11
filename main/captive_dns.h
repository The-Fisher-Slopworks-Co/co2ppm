// SPDX-FileCopyrightText: 2026 The Fisher Slopworks Co
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <stdint.h>

// Minimal captive-portal DNS server.
//
// While the device is in WiFi setup mode it runs a SoftAP. This server answers
// EVERY DNS query with a single A record pointing at resolve_addr (the AP's own
// gateway IP), so any hostname a connected client looks up — including the OS
// connectivity-check domains — resolves to the device. The HTTP server then
// redirects those probes to the setup page, which makes phones and laptops pop
// up the "sign in to network" captive portal automatically.
//
// resolve_addr is an IPv4 address in network byte order (e.g. straight from
// esp_netif_ip_info_t.ip.addr). Both calls are idempotent.
void captive_dns_start(uint32_t resolve_addr);
void captive_dns_stop(void);
