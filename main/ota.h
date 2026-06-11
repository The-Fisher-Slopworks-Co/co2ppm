// SPDX-FileCopyrightText: 2026 The Fisher Slopworks Co
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "esp_http_server.h"

// Over-the-air firmware update over HTTP. Registers two routes on an
// already-started server:
//
//   GET  /update   upload page (password field + .bin file picker)
//   POST /update   receives the raw firmware image, flashes the inactive OTA
//                  slot, validates it, sets it as boot target, and reboots
//
// POST requires the X-OTA-Password header to equal CONFIG_OTA_PASSWORD, so a
// browser or `curl --data-binary @firmware.bin -H "X-OTA-Password: ..."` works.
// No-op when CONFIG_OTA_ENABLE is off.
void ota_register(httpd_handle_t server);

// If the running image was just OTA-updated and is still on probation, mark it
// valid so the bootloader keeps it; otherwise it rolls back on the next reboot.
// Call once the device is known to be reachable. No-op when not pending or when
// rollback is disabled.
void ota_mark_valid_if_pending(void);
