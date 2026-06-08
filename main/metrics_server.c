// SPDX-FileCopyrightText: 2026 The Fisher Slopworks Co
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Prometheus-compatible HTTP metrics endpoint (esp_http_server port of the
// original ESP8266WebServer code).

#include "metrics_server.h"

#include <math.h>
#include <stdio.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "zyaura.h"

static const char *TAG = "metrics";

static esp_err_t root_handler(httpd_req_t *req) {
    static const char page[] =
        "<html><body><h2>CO2 Exporter</h2>"
        "<p><a href='/metrics'>/metrics</a></p>"
        "</body></html>";
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_sendstr(req, page);
}

static esp_err_t metrics_handler(httpd_req_t *req) {
    float co2  = zyaura_co2();
    float temp = zyaura_temp();

    char body[512];
    size_t n = 0;  // bytes actually written into body (always < sizeof(body))

    if (!isnan(co2) && n < sizeof(body)) {
        int w = snprintf(body + n, sizeof(body) - n,
                         "# HELP co2_ppm CO2 concentration in parts per million\n"
                         "# TYPE co2_ppm gauge\n"
                         "co2_ppm{device=\"%s\"} %.0f\n",
                         CONFIG_DEVICE_NAME, co2);
        // snprintf returns the would-be length; clamp to what actually fit.
        n += (w > 0) ? ((size_t)w < sizeof(body) - n ? (size_t)w : sizeof(body) - n - 1) : 0;
    }
    if (!isnan(temp) && n < sizeof(body)) {
        int w = snprintf(body + n, sizeof(body) - n,
                         "# HELP temperature_celsius Temperature in degrees Celsius\n"
                         "# TYPE temperature_celsius gauge\n"
                         "temperature_celsius{device=\"%s\"} %.2f\n",
                         CONFIG_DEVICE_NAME, temp);
        n += (w > 0) ? ((size_t)w < sizeof(body) - n ? (size_t)w : sizeof(body) - n - 1) : 0;
    }

    httpd_resp_set_type(req, "text/plain; version=0.0.4; charset=utf-8");
    if (n == 0) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "# no data yet\n");
    }
    // body is always NUL-terminated within bounds by snprintf.
    return httpd_resp_sendstr(req, body);
}

void metrics_server_begin(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "failed to start HTTP server");
        return;
    }

    httpd_uri_t root = {
        .uri = "/", .method = HTTP_GET, .handler = root_handler,
    };
    httpd_uri_t metrics = {
        .uri = "/metrics", .method = HTTP_GET, .handler = metrics_handler,
    };
    httpd_register_uri_handler(server, &root);
    httpd_register_uri_handler(server, &metrics);

    ESP_LOGI(TAG, "HTTP server started on port %d", config.server_port);
}
