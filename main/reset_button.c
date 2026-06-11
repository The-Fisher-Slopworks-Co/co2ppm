// SPDX-FileCopyrightText: 2026 The Fisher Slopworks Co
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Long-press factory reset button (see reset_button.h). A small task polls the
// button; once it has been held continuously for the configured time it clears
// the stored WiFi credentials and reboots into the setup portal.

#include "reset_button.h"

#include "sdkconfig.h"

#if CONFIG_RESET_BUTTON_ENABLE

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "wifi.h"

static const char *TAG = "reset";

#define BTN_GPIO  CONFIG_PIN_RESET_BUTTON
#define HOLD_MS   (CONFIG_RESET_HOLD_SECONDS * 1000)
#define POLL_MS   50

static void reset_button_task(void *arg) {
    int held_ms = 0;
    for (;;) {
        bool pressed = gpio_get_level(BTN_GPIO) == 0;  // active-low (pull-up)
        if (!pressed) {
            held_ms = 0;
        } else {
            held_ms += POLL_MS;
            if (held_ms >= HOLD_MS) {
                ESP_LOGW(TAG, "factory reset: clearing credentials and rebooting");
                wifi_clear_credentials();
                esp_restart();  // comes back up with no credentials -> setup
            }
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

void reset_button_begin(void) {
    gpio_config_t btn = {
        .pin_bit_mask = 1ULL << BTN_GPIO,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&btn);

    if (xTaskCreate(reset_button_task, "reset_btn", 3072, NULL, 4, NULL) != pdPASS) {
        ESP_LOGW(TAG, "failed to start reset button task");
    }
}

#else  // !CONFIG_RESET_BUTTON_ENABLE

void reset_button_begin(void) {}

#endif
