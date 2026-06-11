// SPDX-FileCopyrightText: 2026 The Fisher Slopworks Co
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Web-push OTA (see ota.h). The POST body IS the firmware image; it is streamed
// straight into the inactive OTA slot, validated by esp_ota_end(), then made the
// boot target. With rollback enabled the new image stays on probation until
// ota_mark_valid_if_pending() confirms it, so a broken update reverts itself.

#include "ota.h"

#include "sdkconfig.h"

#if CONFIG_OTA_ENABLE

#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"

static const char *TAG = "ota";

static const char update_page[] =
    "<!doctype html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Firmware update</title></head>"
    "<body style='font-family:sans-serif;max-width:24em;margin:2em auto;padding:0 1em'>"
    "<h2>CO2 Exporter \xE2\x80\x94 Firmware update</h2>"
    "<p>Password:<br><input id='pw' type='password' style='width:100%;box-sizing:border-box'></p>"
    "<p>Firmware (.bin):<br><input id='f' type='file' accept='.bin'></p>"
    "<p><button onclick='up()'>Upload &amp; flash</button></p>"
    "<p id='s'></p>"
    "<script>"
    "function up(){"
    "var f=document.getElementById('f').files[0];var s=document.getElementById('s');"
    "if(!f){s.textContent='Pick a .bin first';return;}"
    "var x=new XMLHttpRequest();x.open('POST','/update');"
    "x.setRequestHeader('X-OTA-Password',document.getElementById('pw').value);"
    "x.upload.onprogress=function(e){if(e.lengthComputable)"
    "s.textContent='Uploading '+Math.round(e.loaded/e.total*100)+'%';};"
    "x.onload=function(){s.textContent=x.status==200?"
    "'OK \xE2\x80\x94 rebooting, reconnect in ~10 s':'Error: '+x.responseText;};"
    "x.onerror=function(){s.textContent='Upload failed';};"
    "s.textContent='Uploading\xE2\x80\xA6';x.send(f);"
    "}"
    "</script></body></html>";

static esp_err_t update_get(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, update_page, HTTPD_RESP_USE_STRLEN);
}

static bool auth_ok(httpd_req_t *req) {
    if (CONFIG_OTA_PASSWORD[0] == '\0') return false;  // unset password: deny all
    char got[64] = {0};
    if (httpd_req_get_hdr_value_str(req, "X-OTA-Password", got, sizeof(got)) != ESP_OK) {
        return false;
    }
    return strcmp(got, CONFIG_OTA_PASSWORD) == 0;
}

static void restart_cb(void *arg) { esp_restart(); }

// Abort the in-progress write (if any), log, and send an error response.
static esp_err_t fail(httpd_req_t *req, esp_ota_handle_t handle,
                      httpd_err_code_t code, const char *msg) {
    if (handle) esp_ota_abort(handle);
    ESP_LOGE(TAG, "update failed: %s", msg);
    httpd_resp_send_err(req, code, msg);
    return ESP_FAIL;
}

static esp_err_t update_post(httpd_req_t *req) {
    if (!auth_ok(req)) {
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "bad OTA password");
        return ESP_FAIL;
    }
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (part == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no OTA partition");
        return ESP_FAIL;
    }
    if (req->content_len == 0 || req->content_len > part->size) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad image size");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "writing %zu bytes to %s", req->content_len, part->label);

    esp_ota_handle_t handle = 0;
    esp_err_t err = esp_ota_begin(part, req->content_len, &handle);
    if (err != ESP_OK) {
        return fail(req, 0, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
    }

    char buf[1024];
    size_t remaining = req->content_len;
    while (remaining > 0) {
        size_t want = remaining < sizeof(buf) ? remaining : sizeof(buf);
        int r = httpd_req_recv(req, buf, want);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) return fail(req, handle, HTTPD_400_BAD_REQUEST, "recv failed");
        if (esp_ota_write(handle, buf, r) != ESP_OK) {
            return fail(req, handle, HTTPD_500_INTERNAL_SERVER_ERROR, "flash write failed");
        }
        remaining -= r;
    }

    err = esp_ota_end(handle);  // verifies the image; frees the handle on any outcome
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "image invalid: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "image validation failed");
        return ESP_FAIL;
    }
    if (esp_ota_set_boot_partition(part) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "set boot partition failed");
        return ESP_FAIL;
    }

    // Reply, then reboot from a one-shot timer so the response flushes first
    // (resetting straight after sendstr() often tears the socket down too early).
    ESP_LOGW(TAG, "update complete, rebooting into %s", part->label);
    httpd_resp_sendstr(req, "OK");
    const esp_timer_create_args_t reboot = {.callback = restart_cb, .name = "ota_reboot"};
    esp_timer_handle_t timer;
    if (esp_timer_create(&reboot, &timer) == ESP_OK) {
        esp_timer_start_once(timer, 1000000);  // 1 s
    } else {
        esp_restart();  // fallback if the timer can't be created
    }
    return ESP_OK;
}

void ota_register(httpd_handle_t server) {
    const httpd_uri_t get  = {.uri = "/update", .method = HTTP_GET,  .handler = update_get};
    const httpd_uri_t post = {.uri = "/update", .method = HTTP_POST, .handler = update_post};
    httpd_register_uri_handler(server, &get);
    httpd_register_uri_handler(server, &post);
}

void ota_mark_valid_if_pending(void) {
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY) {
        if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
            ESP_LOGI(TAG, "running image confirmed valid");
        }
    }
}

#else  // !CONFIG_OTA_ENABLE

void ota_register(httpd_handle_t server) { (void)server; }
void ota_mark_valid_if_pending(void) {}

#endif
