// SPDX-FileCopyrightText: 2026 The Fisher Slopworks Co
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// On-board status LED.
//
// A single LED conveys the firmware's boot/runtime state through distinct
// blink patterns, so the device's health is readable at a glance without a
// serial monitor:
//
//   BOOT    solid on              early init (NVS/netif/sensor)
//   SETUP   slow blink (1 Hz)     setup portal up, waiting to be configured
//   WIFI    fast blink (~5 Hz)    connecting / no WiFi link
//   ONLINE  double blink + pause  WiFi up, waiting for the first sensor frame
//   OK      brief heartbeat flash WiFi up and sensor data is flowing
//
// Call status_led_begin() once at startup; it configures the GPIO and starts a
// background task that renders the current state. status_led_set() then just
// records the desired state — it is cheap and safe to call often (e.g. from the
// sensor loop). When CONFIG_STATUS_LED_ENABLE is off, both calls are no-ops.

typedef enum {
    STATUS_LED_BOOT,    // early init, before WiFi
    STATUS_LED_SETUP,   // setup portal up, awaiting WiFi credentials
    STATUS_LED_WIFI,    // connecting to WiFi / link down
    STATUS_LED_ONLINE,  // WiFi up, no sensor data yet
    STATUS_LED_OK,      // WiFi up and sensor data available
} status_led_t;

// Configure the LED GPIO and start the renderer task (initial state: BOOT).
void status_led_begin(void);

// Record the state the LED should display. Returns immediately; the renderer
// task picks up the change at the next pattern step.
void status_led_set(status_led_t status);
