// SPDX-FileCopyrightText: 2026 The Fisher Slopworks Co
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// CO2 Exporter — ESP-IDF entry point.
//
// Reads CO2/temperature from a ZyAura ZG-01 (Voltcraft CO-100 and compatible)
// and exposes them as Prometheus metrics over HTTP. Port of the original
// ESP8266/Arduino firmware; the Arduino setup()/loop() model becomes app_main()
// plus a dedicated sensor task.

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "metrics_server.h"
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

static void init_sntp(void) {
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    ESP_ERROR_CHECK(esp_netif_sntp_init(&config));
    if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(10000)) != ESP_OK) {
        ESP_LOGW(TAG, "NTP sync timed out (quiet hours may be off until it syncs)");
    } else {
        ESP_LOGI(TAG, "NTP synced");
    }
}

static void sensor_task(void *arg) {
    for (;;) {
        zyaura_loop();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "CO2 meter starting...");

    init_nvs();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    zyaura_begin(CONFIG_PIN_CLK, CONFIG_PIN_DATA, CONFIG_PIN_BUZZER);
    wifi_begin();
    init_sntp();
    metrics_server_begin();

    xTaskCreate(sensor_task, "sensor", 4096, NULL, 5, NULL);
}
