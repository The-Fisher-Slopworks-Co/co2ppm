// SPDX-FileCopyrightText: 2026 The Fisher Slopworks Co
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// ZyAura ZG-01 sensor driver + buzzer alarm.
// Protocol: https://revspace.nl/CO2MeterHacking
//
// Faithful port of the original Arduino/ESP8266 driver. The bit-banging and
// the buzzer/alarm state machine are logically unchanged; only the platform
// calls differ:
//   attachInterrupt    -> gpio_install_isr_service + gpio_isr_handler_add
//   digitalRead        -> gpio_get_level
//   digitalWrite       -> gpio_set_level
//   micros()/millis()  -> esp_timer_get_time()
//   noInterrupts()     -> portENTER_CRITICAL (spinlock shared with the ISR)
//
// The GPIO ISR is intentionally installed WITHOUT ESP_INTR_FLAG_IRAM: the only
// flash-cache-disabling events are rare, and a dropped clock edge self-heals
// via the gap-reset + checksum, so the ISR may live in flash.

#include "zyaura.h"

#include <math.h>
#include <string.h>
#include <time.h>

#include "driver/gpio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "sdkconfig.h"

// --- ZG-01 frame constants ---
#define ID_TEMPERATURE  0x42  // 'B'  units: 1/16 K
#define ID_CO2          0x50  // 'P'  units: ppm
#define FRAME_BYTES     5
#define GAP_RESET_US    2000

// --- Buzzer timing (not user-tunable; matches the original config.h) ---
#define BEEP_ON_MS         500    // buzzer on duration per beep
#define BEEP_OFF_MS        500    // pause between beeps
#define BEEPS_PER_ALARM    3      // beeps per alarm event
#define ALARM_INTERVAL_MS  (5UL * 60 * 1000)  // min interval between alarms

// --- Module state ---
static int g_pin_data;
static int g_pin_buzzer;

// ISR state (guarded by s_mux, shared with the sensor task)
static portMUX_TYPE      s_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint8_t  rxFrame[FRAME_BYTES];
static volatile uint8_t  bitPos     = 0;
static volatile uint8_t  bytePos    = 0;
static volatile uint8_t  curByte    = 0;
static volatile bool     frameReady = false;
static volatile uint32_t lastEdgeUs = 0;

// Measurements
static float lastCO2ppm = NAN;
static float lastTempC  = NAN;

// Buzzer state machine
enum BuzzerState { BZ_IDLE, BZ_ON, BZ_OFF };
static enum BuzzerState buzzerState = BZ_IDLE;
static uint8_t          beepsDone    = 0;
static uint32_t         buzzerTimer  = 0;
static uint32_t         lastAlarmMs  = 0;
static bool             alarmEver    = false;
static uint32_t         co2HighSince = 0;  // millis() when CO2 first exceeded threshold (0 = not high)

// ---------------------------------------------------------------------------
// Time helpers (Arduino millis()/micros() equivalents)
// ---------------------------------------------------------------------------
static inline uint32_t millis(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
static inline uint32_t micros(void) { return (uint32_t)esp_timer_get_time(); }

// ---------------------------------------------------------------------------
// ISR — one falling clock edge clocks in one data bit
// ---------------------------------------------------------------------------
static void onClock(void *arg) {
    portENTER_CRITICAL_ISR(&s_mux);
    uint32_t now = micros();
    if ((now - lastEdgeUs) > GAP_RESET_US) {
        bitPos  = 0;
        bytePos = 0;
        curByte = 0;
    }
    lastEdgeUs = now;
    curByte = (curByte << 1) | (gpio_get_level(g_pin_data) ? 1 : 0);
    if (++bitPos == 8) {
        if (bytePos < FRAME_BYTES) rxFrame[bytePos++] = curByte;
        bitPos  = 0;
        curByte = 0;
        if (bytePos == FRAME_BYTES) {
            frameReady = true;
            bytePos    = 0;
        }
    }
    portEXIT_CRITICAL_ISR(&s_mux);
}

// ---------------------------------------------------------------------------
// Frame handling
// ---------------------------------------------------------------------------
static bool validateFrame(const uint8_t f[]) {
    return f[4] == 0x0D && (uint8_t)(f[0] + f[1] + f[2]) == f[3];
}

static void processFrame(const uint8_t f[]) {
    uint16_t raw = ((uint16_t)f[1] << 8) | f[2];
    switch (f[0]) {
        case ID_CO2:         lastCO2ppm = raw;                    break;
        case ID_TEMPERATURE: lastTempC  = raw / 16.0f - 273.15f; break;
    }
}

// ---------------------------------------------------------------------------
// Buzzer
// ---------------------------------------------------------------------------
static bool isQuietHour(void) {
    time_t now = time(NULL);
    if (now < 1000000000L) return false;  // NTP not synced
    // Local time without messing with POSIX TZ strings: shift by the raw
    // offset and read the broken-down value as if it were UTC.
    time_t local = now + (time_t)CONFIG_TZ_OFFSET_HOURS * 3600;
    struct tm t;
    gmtime_r(&local, &t);
    return t.tm_hour >= CONFIG_QUIET_HOUR_START || t.tm_hour < CONFIG_QUIET_HOUR_END;
}

static void updateBuzzer(void) {
    if (buzzerState == BZ_IDLE) return;
    uint32_t duration = (buzzerState == BZ_ON) ? BEEP_ON_MS : BEEP_OFF_MS;
    if (millis() - buzzerTimer < duration) return;

    buzzerTimer = millis();
    if (buzzerState == BZ_ON) {
        gpio_set_level(g_pin_buzzer, 0);
        if (++beepsDone >= BEEPS_PER_ALARM) {
            buzzerState = BZ_IDLE;
        } else {
            buzzerState = BZ_OFF;
        }
    } else {
        gpio_set_level(g_pin_buzzer, 1);
        buzzerState = BZ_ON;
    }
}

static void checkAlarm(void) {
    if (isnan(lastCO2ppm) || lastCO2ppm <= CONFIG_CO2_ALARM_PPM) {
        co2HighSince = 0;  // reset on drop below threshold
        return;
    }
    if (co2HighSince == 0) co2HighSince = millis();
    if (millis() - co2HighSince < (uint32_t)CONFIG_CO2_SUSTAIN_MS) return;  // not sustained long enough

    if (isQuietHour()) return;
    if (buzzerState != BZ_IDLE) return;
    if (alarmEver && millis() - lastAlarmMs < ALARM_INTERVAL_MS) return;

    alarmEver   = true;
    lastAlarmMs = millis();
    beepsDone   = 0;
    buzzerTimer = millis();
    buzzerState = BZ_ON;
    gpio_set_level(g_pin_buzzer, 1);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void zyaura_begin(int pin_clk, int pin_data, int pin_buzzer) {
    g_pin_data   = pin_data;
    g_pin_buzzer = pin_buzzer;

    gpio_config_t clk = {
        .pin_bit_mask = 1ULL << pin_clk,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_NEGEDGE,        // FALLING edge
    };
    gpio_config(&clk);

    gpio_config_t dat = {
        .pin_bit_mask = 1ULL << pin_data,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,       // INPUT_PULLUP
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&dat);

    gpio_config_t bz = {
        .pin_bit_mask = 1ULL << pin_buzzer,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&bz);
    gpio_set_level(g_pin_buzzer, 0);

    gpio_install_isr_service(0);  // flags = 0: handler may run from flash
    gpio_isr_handler_add(pin_clk, onClock, NULL);
}

void zyaura_loop(void) {
    updateBuzzer();
    checkAlarm();

    uint8_t f[FRAME_BYTES];
    bool have = false;

    portENTER_CRITICAL(&s_mux);
    bool stale = (bytePos > 0) && ((micros() - lastEdgeUs) > GAP_RESET_US);
    if (stale) { bitPos = 0; bytePos = 0; curByte = 0; }

    if (frameReady) {
        memcpy(f, (const void *)rxFrame, FRAME_BYTES);
        frameReady = false;
        have = true;
    }
    portEXIT_CRITICAL(&s_mux);

    if (have && validateFrame(f)) processFrame(f);
}

float zyaura_co2(void)  { return lastCO2ppm; }
float zyaura_temp(void) { return lastTempC;  }
