// SPDX-FileCopyrightText: 2026 The Fisher Slopworks Co
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// CO2 Exporter — ESP-IDF entry point.
//
// Reads CO2/temperature from a ZyAura ZG-01 (Voltcraft CO-100 and compatible)
// and exposes them as Prometheus metrics over HTTP. Port of the original
// ESP8266/Arduino firmware; the Arduino setup()/loop() model becomes app_main()
// plus a dedicated sensor task.

#include <math.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "metrics_server.h"
#include "reset_button.h"
#include "status_led.h"
#include "wifi.h"
#include "zyaura.h"

static const char *TAG = "co2";

static void init_nvs(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

// Translate the live WiFi/sensor state into a status LED pattern.
static status_led_t current_status(void) {
    if (wifi_in_setup())      return STATUS_LED_SETUP;
    if (!wifi_is_connected()) return STATUS_LED_WIFI;
    if (isnan(zyaura_co2()))  return STATUS_LED_ONLINE;
    return STATUS_LED_OK;
}

static void sensor_task(void *arg) {
    for (;;) {
        zyaura_loop();
        status_led_set(current_status());
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "CO2 meter starting...");

    status_led_begin();  // solid on while we bring the system up

    init_nvs();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    zyaura_begin(CONFIG_PIN_CLK, CONFIG_PIN_DATA);

    status_led_set(STATUS_LED_WIFI);  // until the sensor task starts reporting state
    wifi_begin();         // non-blocking: events drive connect/setup transitions
    metrics_server_begin();  // serves /metrics and the WiFi setup page
    reset_button_begin();    // long-press BOOT to wipe credentials and reboot

    xTaskCreate(sensor_task, "sensor", 4096, NULL, 5, NULL);
}
