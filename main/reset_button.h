// SPDX-FileCopyrightText: 2026 The Fisher Slopworks Co
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// Long-press factory reset on a push button (the on-board BOOT button / GPIO0
// by default). Holding it for CONFIG_RESET_HOLD_SECONDS erases the stored WiFi
// credentials and reboots, so the device comes back up in setup mode.
//
// The button is sampled only at runtime, never at reset, so it does not
// interfere with GPIO0's boot-strapping role (holding BOOT during power-on still
// enters the serial bootloader, as usual — that is separate from this feature).
//
// Call once at startup, after NVS is initialised. No-op when
// CONFIG_RESET_BUTTON_ENABLE is off.
void reset_button_begin(void);
