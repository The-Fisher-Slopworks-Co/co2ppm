// SPDX-FileCopyrightText: 2026 The Fisher Slopworks Co
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once
#include <stdint.h>

// ZyAura ZG-01 sensor driver (ESP-IDF port).
//
// Call zyaura_begin() once at startup, then zyaura_loop() repeatedly from a
// task (e.g. every 10 ms). The clock line is sampled by a GPIO interrupt; the
// loop only validates completed frames.

void  zyaura_begin(int pin_clk, int pin_data);
void  zyaura_loop(void);

float zyaura_co2(void);   // ppm, or NAN if no data yet
float zyaura_temp(void);  // degrees C, or NAN if no data yet
