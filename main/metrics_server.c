// SPDX-FileCopyrightText: 2026 The Fisher Slopworks Co
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// HTTP server (esp_http_server). Serves the Prometheus /metrics endpoint and a
// WiFi setup page. The setup page is reachable both on the setup SoftAP (where
// a captive-portal catch-all redirects every request to it) and on the running
// station at port 80, so credentials can be changed without reflashing.

#include "metrics_server.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "wifi.h"
#include "zyaura.h"

static const char *TAG = "metrics";

// ---------------------------------------------------------------------------
// Small text helpers
// ---------------------------------------------------------------------------

// printf-append into buf at *off, clamping to cap. snprintf returns the length
// it WOULD have written, so advancing *off by it unclamped can run *off past
// cap and make the next "cap - *off" underflow; this keeps *off <= cap-1.
static void appendf(char *buf, size_t cap, size_t *off, const char *fmt, ...) {
    if (*off >= cap) return;
    va_list ap;
    va_start(ap, fmt);
    int w = vsnprintf(buf + *off, cap - *off, fmt, ap);
    va_end(ap);
    if (w > 0) {
        *off += ((size_t)w < cap - *off) ? (size_t)w : cap - *off - 1;
    }
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Decode an application/x-www-form-urlencoded token in place of httpd's raw
// extraction (httpd_query_key_value does not undo percent-encoding or '+').
static void url_decode(char *dst, size_t cap, const char *src) {
    size_t di = 0;
    for (size_t si = 0; src[si] != '\0' && di + 1 < cap; si++) {
        char c = src[si];
        if (c == '+') {
            c = ' ';
        } else if (c == '%') {
            int hi = hexval(src[si + 1]);
            int lo = hi < 0 ? -1 : hexval(src[si + 2]);
            if (lo >= 0) {
                c = (char)((hi << 4) | lo);
                si += 2;
            }
        }
        dst[di++] = c;
    }
    dst[di] = '\0';
}

// Append src to dst (HTML-escaping &, <, >, ") starting at *off within cap.
static void html_escape_append(char *dst, size_t cap, size_t *off, const char *src) {
    for (size_t i = 0; src[i] != '\0'; i++) {
        const char *rep = NULL;
        switch (src[i]) {
            case '&': rep = "&amp;";  break;
            case '<': rep = "&lt;";   break;
            case '>': rep = "&gt;";   break;
            case '"': rep = "&quot;"; break;
            default:  break;
        }
        if (rep) {
            size_t rl = strlen(rep);
            if (*off + rl >= cap) break;
            memcpy(dst + *off, rep, rl);
            *off += rl;
        } else {
            if (*off + 1 >= cap) break;
            dst[(*off)++] = src[i];
        }
    }
    dst[*off] = '\0';
}

// ---------------------------------------------------------------------------
// Metrics
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// WiFi setup page
// ---------------------------------------------------------------------------

// Append a <select> of nearby networks; choosing one fills the SSID field via a
// tiny inline handler (more reliable on mobile than a <datalist>). When the scan
// finds nothing, leave a hint to type the SSID manually instead.
static void append_network_picker(char *buf, size_t cap, size_t *off) {
    char ssids[12][WIFI_SSID_MAXLEN];
    int n = wifi_scan_ssids(ssids, 12);
    if (n <= 0) {
        appendf(buf, cap, off,
            "<p><small>No networks found by scan \xE2\x80\x94 type the SSID above.</small></p>");
        return;
    }

    appendf(buf, cap, off,
        "<p>or pick a detected network:<br>"
        "<select onchange=\"if(this.value)document.getElementById('ssid').value=this.value\""
        " style='width:100%%;box-sizing:border-box'>"
        "<option value=''>\xE2\x80\x94</option>");
    for (int i = 0; i < n && *off < cap - 96; i++) {
        appendf(buf, cap, off, "<option value=\"");
        html_escape_append(buf, cap, off, ssids[i]);
        appendf(buf, cap, off, "\">");
        html_escape_append(buf, cap, off, ssids[i]);
        appendf(buf, cap, off, "</option>");
    }
    appendf(buf, cap, off, "</select></p>");
}

static esp_err_t setup_get_handler(httpd_req_t *req) {
    char ssid[33] = {0};
    wifi_get_ssid(ssid, sizeof(ssid));

    const size_t cap = 3072;
    char *page = malloc(cap);
    if (page == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_FAIL;
    }

    size_t off = 0;
    appendf(page, cap, &off,
        "<!doctype html><html><head>"
        "<meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>WiFi setup</title></head>"
        "<body style='font-family:sans-serif;max-width:24em;margin:2em auto;padding:0 1em'>"
        "<h2>CO2 Exporter \xE2\x80\x94 WiFi setup</h2>"
        "<form method='POST' action='/setup'>"
        "<p>Network (SSID):<br>"
        "<input id='ssid' name='ssid' autofocus style='width:100%%;box-sizing:border-box' value=\"");
    html_escape_append(page, cap, &off, ssid);
    appendf(page, cap, &off, "\"></p>");

    append_network_picker(page, cap, &off);

    appendf(page, cap, &off,
        "<p>Password:<br>"
        "<input name='password' type='password' style='width:100%%;box-sizing:border-box'>"
        "<br><small>Leave empty for an open network.</small></p>"
        "<p><button type='submit'>Save &amp; connect</button></p>"
        "</form></body></html>");

    httpd_resp_set_type(req, "text/html");
    esp_err_t err = httpd_resp_sendstr(req, page);
    free(page);
    return err;
}

static esp_err_t setup_post_handler(httpd_req_t *req) {
    char buf[256];
    int total = req->content_len;
    if (total <= 0 || total >= (int)sizeof(buf)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad form");
        return ESP_FAIL;
    }

    int received = 0;
    while (received < total) {
        int r = httpd_req_recv(req, buf + received, total - received);
        if (r <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv failed");
            return ESP_FAIL;
        }
        received += r;
    }
    buf[received] = '\0';

    char ssid_raw[96] = {0};
    char pass_raw[160] = {0};
    char ssid[33] = {0};
    char pass[65] = {0};

    if (httpd_query_key_value(buf, "ssid", ssid_raw, sizeof(ssid_raw)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing ssid");
        return ESP_FAIL;
    }
    httpd_query_key_value(buf, "password", pass_raw, sizeof(pass_raw));  // optional

    url_decode(ssid, sizeof(ssid), ssid_raw);
    url_decode(pass, sizeof(pass), pass_raw);

    if (ssid[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty ssid");
        return ESP_FAIL;
    }

    // Reply BEFORE applying: applying may drop this very AP / connection.
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req,
        "<!doctype html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'></head>"
        "<body style='font-family:sans-serif;max-width:24em;margin:2em auto;padding:0 1em'>"
        "<h2>Saved</h2><p>Connecting to the network. If it succeeds, this setup "
        "network disappears \xE2\x80\x94 reconnect to your WiFi and reach the device "
        "by its hostname. If the credentials are wrong, the setup network comes "
        "back shortly.</p></body></html>");

    ESP_LOGI(TAG, "setup: new SSID \"%s\"", ssid);
    wifi_apply_credentials(ssid, pass);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Root + captive-portal catch-all
// ---------------------------------------------------------------------------

static esp_err_t redirect_to_setup(httpd_req_t *req) {
    // Relative target: the captive DNS already steers every hostname to this
    // device, so "/setup" is correct regardless of the AP's gateway IP.
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/setup");
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, "redirecting to WiFi setup\n");
}

static esp_err_t root_handler(httpd_req_t *req) {
    if (wifi_in_setup()) return redirect_to_setup(req);

    static const char page[] =
        "<html><body><h2>CO2 Exporter</h2>"
        "<p><a href='/metrics'>/metrics</a></p>"
        "<p><a href='/setup'>Configure WiFi</a></p>"
        "</body></html>";
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_sendstr(req, page);
}

// Matches anything not matched by a more specific handler. In setup mode it
// redirects OS connectivity probes to the portal (triggering the captive popup);
// otherwise it is a plain 404.
static esp_err_t catchall_handler(httpd_req_t *req) {
    if (wifi_in_setup()) return redirect_to_setup(req);
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
    return ESP_FAIL;
}

// ---------------------------------------------------------------------------

void metrics_server_begin(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port  = 80;
    config.uri_match_fn = httpd_uri_match_wildcard;  // enables the "/*" catch-all

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "failed to start HTTP server");
        return;
    }

    // Register specific routes before the wildcard; first match wins.
    const httpd_uri_t routes[] = {
        {.uri = "/metrics", .method = HTTP_GET,  .handler = metrics_handler},
        {.uri = "/setup",   .method = HTTP_GET,  .handler = setup_get_handler},
        {.uri = "/setup",   .method = HTTP_POST, .handler = setup_post_handler},
        {.uri = "/",        .method = HTTP_GET,  .handler = root_handler},
        {.uri = "/*",       .method = HTTP_GET,  .handler = catchall_handler},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        httpd_register_uri_handler(server, &routes[i]);
    }

    ESP_LOGI(TAG, "HTTP server started on port %d", config.server_port);
}
