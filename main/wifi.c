// SPDX-FileCopyrightText: 2026 The Fisher Slopworks Co
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// WiFi station with NVS-stored credentials and a self-healing setup fallback.
// See wifi.h for the behaviour contract.
//
// The fallback is driven entirely by WiFi events plus one esp_timer:
//
//   STA_DISCONNECTED -> reconnect, and arm a one-shot grace timer (once).
//   grace timer fires while still offline -> enter setup (raise the SoftAP).
//   GOT_IP -> cancel the timer and leave setup (drop the SoftAP).
//
// Because the station keeps retrying throughout, a transient outage of the real
// network reconnects on its own and tears the setup AP back down — the device
// is never stuck in setup mode while the saved network is actually reachable.

#include "wifi.h"

#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "nvs.h"
#include "sdkconfig.h"

#include "captive_dns.h"

static const char *TAG = "wifi";

// Internal event used to hop the grace-timer's setup decision onto the event
// loop task, so enter_setup()/exit_setup() only ever run on one task.
ESP_EVENT_DEFINE_BASE(APP_WIFI_EVENT);
enum { APP_WIFI_GRACE_EXPIRED };

#define WIFI_CONNECTED_BIT BIT0

#define NVS_NAMESPACE "wifi"
#define NVS_KEY_SSID  "ssid"
#define NVS_KEY_PASS  "pass"

#define GRACE_US ((int64_t)CONFIG_WIFI_SETUP_GRACE_SECONDS * 1000000)

static EventGroupHandle_t s_wifi_events;
static esp_netif_t       *s_sta_netif;
static esp_netif_t       *s_ap_netif;
static esp_timer_handle_t s_grace_timer;

static volatile bool s_have_creds;    // a station SSID is configured
static volatile bool s_setup_active;  // the setup SoftAP is up
static volatile bool s_grace_armed;   // grace timer is counting down
static volatile bool s_scanning;      // a scan paused the reconnect loop
static char          s_ssid[33];      // currently configured SSID (for prefill)

// ---------------------------------------------------------------------------
// NVS credential storage
// ---------------------------------------------------------------------------

static bool creds_load(char *ssid, size_t ssid_cap, char *pass, size_t pass_cap) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return false;

    size_t sl = ssid_cap;
    esp_err_t err = nvs_get_str(h, NVS_KEY_SSID, ssid, &sl);
    if (err == ESP_OK) {
        size_t pl = pass_cap;
        if (nvs_get_str(h, NVS_KEY_PASS, pass, &pl) != ESP_OK) {
            pass[0] = '\0';  // no stored password -> open network
        }
    }
    nvs_close(h);
    return err == ESP_OK && ssid[0] != '\0';
}

static esp_err_t creds_save(const char *ssid, const char *pass) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, NVS_KEY_SSID, ssid);
    if (err == ESP_OK) err = nvs_set_str(h, NVS_KEY_PASS, pass);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

// ---------------------------------------------------------------------------
// Station / SoftAP configuration
// ---------------------------------------------------------------------------

static void sta_set_config(const char *ssid, const char *pass) {
    wifi_config_t wc = {0};
    strncpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, pass, sizeof(wc.sta.password) - 1);
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));

    strncpy(s_ssid, ssid, sizeof(s_ssid) - 1);
    s_ssid[sizeof(s_ssid) - 1] = '\0';
    s_have_creds = ssid[0] != '\0';
}

static void ap_set_config(void) {
    wifi_config_t ap = {0};
    int n = snprintf((char *)ap.ap.ssid, sizeof(ap.ap.ssid), "%s-setup", CONFIG_HOSTNAME);
    ap.ap.ssid_len      = (n > 0 && (size_t)n < sizeof(ap.ap.ssid)) ? n : sizeof(ap.ap.ssid) - 1;
    ap.ap.channel       = 1;
    ap.ap.max_connection = 4;
    ap.ap.authmode      = WIFI_AUTH_OPEN;  // open network, per design
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
}

// ---------------------------------------------------------------------------
// Setup mode (SoftAP + captive portal) — raised/lowered around the live STA
// ---------------------------------------------------------------------------

static void enter_setup(void) {
    if (s_setup_active) return;
    ESP_LOGW(TAG, "no connection; opening setup portal (SoftAP \"%s-setup\")", CONFIG_HOSTNAME);

    // APSTA keeps the station running (so it can recover or scan) alongside the
    // AP. esp_wifi is already started; switching the mode is enough.
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ap_set_config();
    s_setup_active = true;

    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(s_ap_netif, &ip) == ESP_OK) {
        captive_dns_start(ip.ip.addr);
    }
}

static void exit_setup(void) {
    if (!s_setup_active) return;
    ESP_LOGI(TAG, "connected; closing setup portal");
    captive_dns_stop();
    s_setup_active = false;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));  // drops the SoftAP
}

// ---------------------------------------------------------------------------
// Grace timer: one-shot countdown from the first failed/lost connection.
// ---------------------------------------------------------------------------

// Runs on the high-priority esp_timer task. It must NOT touch setup state
// directly (that races the event loop's exit_setup() and can leave the device
// connected yet stuck with the AP up). Hand the decision to the event loop.
static void grace_timer_cb(void *arg) {
    s_grace_armed = false;
    esp_event_post(APP_WIFI_EVENT, APP_WIFI_GRACE_EXPIRED, NULL, 0, 0);
}

// Arm the countdown once. Deliberately does NOT restart if already armed, so a
// burst of reconnect/disconnect events can't keep pushing the deadline out.
static void arm_grace(void) {
    if (s_setup_active || s_grace_armed) return;
    if (esp_timer_start_once(s_grace_timer, GRACE_US) == ESP_OK) {
        s_grace_armed = true;
    }
}

static void cancel_grace(void) {
    esp_timer_stop(s_grace_timer);  // harmless if not running
    s_grace_armed = false;
}

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_have_creds) esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
        // Don't fight a scan: it deliberately disconnects and resumes us after.
        if (s_have_creds && !s_scanning) esp_wifi_connect();
        arm_grace();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
        cancel_grace();
        exit_setup();
    } else if (base == APP_WIFI_EVENT && id == APP_WIFI_GRACE_EXPIRED) {
        if (!wifi_is_connected()) enter_setup();
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void wifi_begin(void) {
    s_wifi_events = xEventGroupCreate();

    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif  = esp_netif_create_default_wifi_ap();
    ESP_ERROR_CHECK(esp_netif_set_hostname(s_sta_netif, CONFIG_HOSTNAME));

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        APP_WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL, NULL));

    const esp_timer_create_args_t targs = {.callback = grace_timer_cb, .name = "wifi_grace"};
    ESP_ERROR_CHECK(esp_timer_create(&targs, &s_grace_timer));

    char ssid[33] = {0};
    char pass[65] = {0};
    bool have = creds_load(ssid, sizeof(ssid), pass, sizeof(pass));

    // Start the station unconditionally: with credentials it connects; without
    // them it just provides the STA interface that setup mode (APSTA) needs.
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    if (have) sta_set_config(ssid, pass);
    ESP_ERROR_CHECK(esp_wifi_start());  // STA_START -> connect (if creds)

    if (have) {
        ESP_LOGI(TAG, "connecting to \"%s\"...", ssid);
    } else {
        ESP_LOGW(TAG, "no stored credentials; opening setup portal");
        enter_setup();
    }
}

void wifi_apply_credentials(const char *ssid, const char *password) {
    ESP_LOGI(TAG, "applying credentials for \"%s\"", ssid);

    // Drop any countdown from the old network so the disconnect we trigger below
    // doesn't let a stale timer open setup mid-switch.
    cancel_grace();

    esp_err_t err = creds_save(ssid, password);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to persist credentials: %s", esp_err_to_name(err));
    }

    sta_set_config(ssid, password);

    // Reconnect with the new credentials. In setup mode we're already APSTA;
    // when connected normally the mode is STA. Either way the station is live.
    // Both calls are best-effort kicks to the state machine, so ignore errors
    // (e.g. disconnect when not yet connected); the event handlers do the work.
    esp_wifi_disconnect();
    esp_wifi_connect();
}

bool wifi_is_connected(void) {
    return s_wifi_events != NULL &&
           (xEventGroupGetBits(s_wifi_events) & WIFI_CONNECTED_BIT) != 0;
}

bool wifi_in_setup(void) {
    return s_setup_active;
}

void wifi_get_ssid(char *out, size_t len) {
    if (len == 0) return;
    strncpy(out, s_ssid, len - 1);
    out[len - 1] = '\0';
}

int wifi_scan_ssids(char ssids[][WIFI_SSID_MAXLEN], int max) {
    if (max <= 0) return 0;

    // While connected, a scan keeps the link and doesn't need the dance below.
    // While disconnected the station may be mid-connect, which makes the scan
    // fail with ESP_ERR_WIFI_STATE — pause the reconnect loop and abort the
    // in-flight attempt first, then restore it afterwards.
    bool connected = wifi_is_connected();
    if (!connected) {
        s_scanning = true;
        esp_wifi_disconnect();
    }

    int count = 0;
    wifi_scan_config_t sc = {0};
    esp_err_t err = esp_wifi_scan_start(&sc, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan failed: %s", esp_err_to_name(err));
    } else {
        uint16_t found = 0;
        esp_wifi_scan_get_ap_num(&found);
        wifi_ap_record_t *recs = found ? calloc(found, sizeof(*recs)) : NULL;
        if (recs != NULL) {
            uint16_t got = found;
            if (esp_wifi_scan_get_ap_records(&got, recs) == ESP_OK) {
                for (uint16_t i = 0; i < got && count < max; i++) {
                    const char *s = (const char *)recs[i].ssid;
                    if (s[0] == '\0') continue;  // hidden network
                    bool dup = false;
                    for (int j = 0; j < count; j++) {
                        if (strcmp(ssids[j], s) == 0) { dup = true; break; }
                    }
                    if (dup) continue;
                    strncpy(ssids[count], s, WIFI_SSID_MAXLEN - 1);
                    ssids[count][WIFI_SSID_MAXLEN - 1] = '\0';
                    count++;
                }
            } else {
                esp_wifi_clear_ap_list();
            }
            free(recs);
        } else {
            esp_wifi_clear_ap_list();  // nothing found, or alloc failed
        }
    }

    if (!connected) {
        s_scanning = false;
        if (s_have_creds) esp_wifi_connect();  // resume reconnect attempts
    }

    ESP_LOGI(TAG, "scan found %d network(s)", count);
    return count;
}

void wifi_clear_credentials(void) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        nvs_erase_all(h);  // the namespace holds only our credentials
        err = nvs_commit(h);
        nvs_close(h);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to clear credentials: %s", esp_err_to_name(err));
    } else {
        ESP_LOGW(TAG, "credentials cleared");
    }
}
