// SPDX-FileCopyrightText: 2026 The Fisher Slopworks Co
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// Starts an HTTP server on port 80 exposing:
//   GET /         small landing page
//   GET /metrics  Prometheus text exposition (text/plain; version=0.0.4)
// The server runs in its own task; there is no loop() to pump.
void metrics_server_begin(void);
