// SPDX-FileCopyrightText: 2026 The Fisher Slopworks Co
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Minimal captive-portal DNS server (see captive_dns.h). A single UDP task
// listens on port 53 and replies to every query with one A record pointing at
// the AP gateway. It deliberately ignores the query type — for a captive portal
// "everything is us" is exactly the desired behaviour.

#include "captive_dns.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

static const char *TAG = "captive";

#define DNS_PORT       53
#define DNS_BUF_LEN    256  // a single A query/response is far smaller than this
#define DNS_HDR_LEN    12

static TaskHandle_t   s_task;
static volatile bool  s_running;
static uint32_t       s_resolve_addr;  // network byte order

// Build a response into tx for the query in rx (rx_len bytes). Returns the
// response length, or 0 if the query is malformed and should be dropped.
static int build_response(const uint8_t *rx, int rx_len, uint8_t *tx, int tx_cap) {
    if (rx_len < DNS_HDR_LEN) return 0;

    // Walk the QNAME labels to find the end of the (single) question section.
    int pos = DNS_HDR_LEN;
    while (pos < rx_len && rx[pos] != 0) {
        pos += rx[pos] + 1;  // length-prefixed label
        if (pos >= rx_len) return 0;  // ran off the end: malformed
    }
    pos += 1;      // skip the zero-length root label
    pos += 4;      // skip QTYPE + QCLASS
    if (pos > rx_len) return 0;

    const int answer_len = 16;  // ptr(2)+type(2)+class(2)+ttl(4)+rdlen(2)+rdata(4)
    if (pos + answer_len > tx_cap) return 0;

    // Header + question are echoed back verbatim, then patched.
    memcpy(tx, rx, pos);
    tx[2] = 0x81;  // QR=1, opcode=0, AA=0, TC=0, RD copied below
    tx[3] = 0x80;  // RA=1, RCODE=0
    tx[2] |= rx[2] & 0x01;  // preserve the client's RD bit
    tx[6] = 0x00; tx[7] = 0x01;  // ANCOUNT = 1
    tx[8] = 0x00; tx[9] = 0x00;  // NSCOUNT = 0
    tx[10] = 0x00; tx[11] = 0x00;  // ARCOUNT = 0 (drop any EDNS OPT)

    uint8_t *a = tx + pos;
    a[0] = 0xC0; a[1] = 0x0C;        // name: pointer to the question's QNAME
    a[2] = 0x00; a[3] = 0x01;        // TYPE  = A
    a[4] = 0x00; a[5] = 0x01;        // CLASS = IN
    a[6] = a[7] = a[8] = a[9] = 0;   // TTL   = 0 (don't let probes cache us)
    a[10] = 0x00; a[11] = 0x04;      // RDLENGTH = 4
    memcpy(a + 12, &s_resolve_addr, 4);  // RDATA = gateway IP (network order)

    return pos + answer_len;
}

static void dns_task(void *arg) {
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket() failed");
        goto done;
    }

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(DNS_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "bind() failed");
        close(sock);
        goto done;
    }

    // Time out recvfrom() once a second so the task can notice s_running going
    // false and exit promptly when setup mode ends.
    struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    ESP_LOGI(TAG, "captive DNS up");
    uint8_t rx[DNS_BUF_LEN];
    uint8_t tx[DNS_BUF_LEN];
    while (s_running) {
        struct sockaddr_in src;
        socklen_t          src_len = sizeof(src);
        int n = recvfrom(sock, rx, sizeof(rx), 0, (struct sockaddr *)&src, &src_len);
        if (n <= 0) continue;  // timeout or error: re-check s_running

        int len = build_response(rx, n, tx, sizeof(tx));
        if (len > 0) {
            sendto(sock, tx, len, 0, (struct sockaddr *)&src, src_len);
        }
    }

    ESP_LOGI(TAG, "captive DNS down");
    close(sock);

done:
    s_running = false;
    s_task    = NULL;
    vTaskDelete(NULL);
}

void captive_dns_start(uint32_t resolve_addr) {
    // s_task lingers for up to one recv timeout after stop(); don't spawn a
    // second task onto the same port while the old one is winding down. In
    // normal use start/stop are tens of seconds apart, so this never blocks.
    if (s_running || s_task != NULL) return;
    s_resolve_addr = resolve_addr;
    s_running      = true;
    if (xTaskCreate(dns_task, "captive_dns", 3072, NULL, 5, &s_task) != pdPASS) {
        ESP_LOGW(TAG, "failed to start DNS task");
        s_running = false;
    }
}

void captive_dns_stop(void) {
    s_running = false;  // the task closes its socket and exits within ~1 s
}
