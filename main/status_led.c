// SPDX-FileCopyrightText: 2026 The Fisher Slopworks Co
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// On-board status LED: a small renderer task drives one GPIO through a blink
// pattern chosen by the current status_led_t. Each pattern is a loop of
// (on/off, milliseconds) steps; the task replays the active pattern and
// restarts from the top whenever the status changes, so transitions show up
// within one step. Long off-steps are waited out in short chunks so a status
// change is never masked by an in-progress delay.

#include "status_led.h"

#include "sdkconfig.h"

#if CONFIG_STATUS_LED_ENABLE

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "led";

#define LED_GPIO        CONFIG_PIN_STATUS_LED
#define POLL_CHUNK_MS   50  // max latency before a status change is noticed

// One step of a blink pattern.
typedef struct {
    bool     on;
    uint16_t ms;
} led_step_t;

// Patterns, one per status. Each plays on a loop; see status_led.h for intent.
static const led_step_t pat_boot[]   = {{true, 250}};
static const led_step_t pat_setup[]  = {{true, 500}, {false, 500}};
static const led_step_t pat_wifi[]   = {{true, 100}, {false, 100}};
static const led_step_t pat_online[] = {{true, 80}, {false, 200}, {true, 80}, {false, 1600}};
static const led_step_t pat_ok[]     = {{true, 60}, {false, 2940}};

typedef struct {
    const led_step_t *steps;
    size_t            len;
} led_pattern_t;

#define PATTERN(arr) {(arr), sizeof(arr) / sizeof((arr)[0])}

// Indexed by status_led_t.
static const led_pattern_t s_patterns[] = {
    [STATUS_LED_BOOT]   = PATTERN(pat_boot),
    [STATUS_LED_SETUP]  = PATTERN(pat_setup),
    [STATUS_LED_WIFI]   = PATTERN(pat_wifi),
    [STATUS_LED_ONLINE] = PATTERN(pat_online),
    [STATUS_LED_OK]     = PATTERN(pat_ok),
};

// Single writer (status_led_set) / single reader (renderer task); a plain
// 32-bit volatile store/load is atomic on the ESP32, so no lock is needed.
static volatile status_led_t s_status = STATUS_LED_BOOT;

static void write_led(bool on) {
#if CONFIG_STATUS_LED_ACTIVE_LOW
    on = !on;
#endif
    gpio_set_level(LED_GPIO, on ? 1 : 0);
}

static void status_led_task(void *arg) {
    status_led_t shown = s_status;
    size_t step = 0;

    for (;;) {
        status_led_t s = s_status;
        if (s != shown) {  // status changed: restart its pattern from the top
            shown = s;
            step  = 0;
        }

        const led_pattern_t *p = &s_patterns[shown];
        const led_step_t    *cur = &p->steps[step % p->len];
        write_led(cur->on);

        // Hold this step, but wake early if the status changes underneath us.
        for (uint16_t waited = 0; waited < cur->ms && s_status == shown;) {
            uint16_t chunk = cur->ms - waited;
            if (chunk > POLL_CHUNK_MS) chunk = POLL_CHUNK_MS;
            vTaskDelay(pdMS_TO_TICKS(chunk));
            waited += chunk;
        }
        step++;
    }
}

void status_led_begin(void) {
    gpio_config_t led = {
        .pin_bit_mask = 1ULL << LED_GPIO,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&led);
    write_led(false);

    if (xTaskCreate(status_led_task, "status_led", 2048, NULL, 4, NULL) != pdPASS) {
        ESP_LOGW(TAG, "failed to start status LED task");
    }
}

void status_led_set(status_led_t status) { s_status = status; }

#else  // !CONFIG_STATUS_LED_ENABLE

void status_led_begin(void) {}
void status_led_set(status_led_t status) { (void)status; }

#endif
