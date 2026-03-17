/**
 * @file wifi_config.c
 * @brief SoftAP + HTTP captive-config portal for the BYUI eBadge.
 *
 * Flow:
 *   1. wifi_config_start() → starts AP "BADGE-CONFIG" (open, ch 1)
 *   2. HTTP server on 192.168.4.1:80  GET / → HTML form
 *                                     POST /save → save SSID+pass to NVS
 *   3. Caller polls wifi_config_done() or waits; then calls wifi_config_stop()
 */

#include "wifi_config.h"

#include <string.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdint.h>

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"

#define TAG "wifi_config"

#define AP_SSID_BASE  "BYUI_NameBadge"
#define AP_CHANNEL    1
#define AP_MAX_CONN   4

static httpd_handle_t  s_server          = NULL;
static volatile bool   s_done            = false;
static volatile bool   s_sta_joined      = false;
static volatile bool   s_form_served     = false;
static esp_netif_t    *s_ap_netif        = NULL;
static TaskHandle_t    s_dns_task        = NULL;
static volatile bool   s_dns_running     = false;

/* ── Wi-Fi scan results (populated once at startup) ─────────────── */
#define MAX_SCAN_APS  20
static wifi_ap_record_t s_scan_aps[MAX_SCAN_APS];
static uint16_t         s_scan_count = 0;

/* ── AP station-connect event ────────────────────────────────────── */
static void ap_event_handler(void *arg, esp_event_base_t base,
                              int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        ESP_LOGI(TAG, "Station joined AP");
        s_sta_joined = true;
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STADISCONNECTED) {
        /* Reset only if no other station is still associated */
        s_sta_joined = false;
    }
}

#define WIFI_CONFIG_NVS_PARTITION  "user_data"

/* ── AP IP + SSID (MAC-derived, computed once in start_softap) ─────── */
/* IP:   192.168.(mac[4] % 240).(mac[5] % 240); last octet ≥ 1.       */
/* SSID: BYUI_NameBadge_XX  where XX = mac[5] as uppercase hex.        */
static uint8_t s_ap_ip[4]    = {192, 168, 4, 1};
static char    s_ap_url[28]  = "http://192.168.4.1/";
static char    s_ap_ssid[20] = AP_SSID_BASE;

/* ── HTML-attribute escaping ─────────────────────────────────────── */

/** Write HTML-escaped src into [*p, end).  Advances *p.  Returns false if truncated. */
static bool html_esc(char **p, const char *end, const char *src)
{
    for (; *src; src++) {
        const char *ent = NULL;
        switch (*src) {
            case '&':  ent = "&amp;";  break;
            case '<':  ent = "&lt;";   break;
            case '>':  ent = "&gt;";   break;
            case '"':  ent = "&quot;"; break;
            case '\'': ent = "&#39;";  break;
            default:   break;
        }
        if (ent) {
            size_t el = strlen(ent);
            if (*p + el >= end) return false;
            memcpy(*p, ent, el);
            *p += el;
        } else {
            if (*p + 1 >= end) return false;
            *(*p)++ = *src;
        }
    }
    return true;
}

/* ── Wi-Fi scan (runs once at startup, AP+STA concurrent mode) ───── */

static void scan_nearby_networks(void)
{
    esp_netif_t *sta = esp_netif_create_default_wifi_sta();

    if (esp_wifi_set_mode(WIFI_MODE_APSTA) != ESP_OK) {
        ESP_LOGW(TAG, "Could not enter APSTA mode for scan");
        esp_netif_destroy(sta);
        return;
    }

    wifi_scan_config_t cfg = {
        .scan_type            = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 100,
        .scan_time.active.max = 250,
    };
    if (esp_wifi_scan_start(&cfg, true) == ESP_OK) {
        s_scan_count = MAX_SCAN_APS;
        esp_wifi_scan_get_ap_records(&s_scan_count, s_scan_aps);
        ESP_LOGI(TAG, "WiFi scan: %u APs found", s_scan_count);
    } else {
        s_scan_count = 0;
        ESP_LOGW(TAG, "WiFi scan failed — SSID field will be free-text");
    }

    /* Return to pure-AP mode and tear down the temporary STA netif. */
    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_netif_destroy(sta);
}

/* ── Captive-portal DNS hijack ──────────────────────────────────── */
/*
 * Listens on UDP port 53 and responds to every DNS query with an
 * A record pointing at 192.168.4.1.  This causes the phone OS to
 * detect a captive portal and automatically pop up the browser.
 */
#define DNS_BUF_SIZE 512

static void dns_server_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "DNS socket create failed");
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in srv = {
        .sin_family      = AF_INET,
        .sin_port        = htons(53),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&srv, sizeof(srv)) < 0) {
        ESP_LOGE(TAG, "DNS bind failed");
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    /* 1-second receive timeout so we can check s_dns_running. */
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    ESP_LOGI(TAG, "DNS hijack task started");

    static uint8_t buf[DNS_BUF_SIZE];
    struct sockaddr_in client;
    socklen_t clen = sizeof(client);

    while (s_dns_running) {
        int n = recvfrom(sock, buf, sizeof(buf) - 20, 0,
                         (struct sockaddr *)&client, &clen);
        if (n < 12) continue;  /* timeout (-1) or truncated — skip */

        /* Turn the query into a response in-place:
         *   QR=1 (response), AA=1 (authoritative), RD copied, RA=1
         *   RCODE = 0 (no error), ANCOUNT = 1                      */
        buf[2] = (uint8_t)((buf[2] & 0x01) | 0x84); /* QR AA RD(kept) */
        buf[3] = 0x80;  /* RA=1, RCODE=0 */
        buf[6] = 0x00; buf[7] = 0x01;  /* ANCOUNT = 1 */
        buf[8] = 0x00; buf[9] = 0x00;  /* NSCOUNT = 0 */
        buf[10]= 0x00; buf[11]= 0x00;  /* ARCOUNT = 0 */

        /* Append answer RR: name ptr → offset 12, type A, class IN,
         * TTL 0 (don't cache), RDLEN 4, address from s_ap_ip         */
        uint8_t ans[16] = {
            0xC0, 0x0C,              /* Name: pointer to offset 12  */
            0x00, 0x01,              /* Type: A                     */
            0x00, 0x01,              /* Class: IN                   */
            0x00, 0x00, 0x00, 0x00, /* TTL: 0                      */
            0x00, 0x04,              /* RDLENGTH: 4                 */
            0, 0, 0, 0               /* RDATA: filled below         */
        };
        ans[12] = s_ap_ip[0];  ans[13] = s_ap_ip[1];
        ans[14] = s_ap_ip[2];  ans[15] = s_ap_ip[3];
        memcpy(buf + n, ans, sizeof(ans));
        sendto(sock, buf, n + sizeof(ans), 0,
               (struct sockaddr *)&client, clen);
    }

    close(sock);
    ESP_LOGI(TAG, "DNS hijack task stopped");
    vTaskDelete(NULL);
}

static void start_dns_server(void)
{
    s_dns_running = true;
    xTaskCreate(dns_server_task, "dns_hijack", 4096, NULL, 5, &s_dns_task);
}

static void stop_dns_server(void)
{
    s_dns_running = false;
    /* Task self-deletes after its next 1 s recv timeout. */
    s_dns_task = NULL;
}

/* ── HTML page (split around the dynamic SSID field) ────────────── */
static const char HTML_FORM_HEAD[] =
    "<!DOCTYPE html><html><head>"
    "<meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>BYUI Badge Setup</title>"
    "<style>"
    "body{font-family:sans-serif;max-width:400px;margin:40px auto;padding:0 16px}"
    "h2{color:#006eb8}"
    "label{display:block;font-weight:bold;font-size:.9em;margin-top:12px}"
    "input,select{width:100%;box-sizing:border-box;padding:8px;margin:4px 0 2px;"
          "border:1px solid #ccc;border-radius:4px}"
    "hr{border:none;border-top:1px solid #e0e0e0;margin:18px 0}"
    "button{margin-top:18px;width:100%;padding:12px;background:#006eb8;"
           "color:#fff;border:none;border-radius:4px;font-size:1em;cursor:pointer}"
    "</style></head><body>"
    "<h2>BYUI eBadge Setup</h2>"
    "<form method='POST' action='/save'>"
    "<label>Badge Nickname</label>"
    "<input name='nick' placeholder='e.g. Jane Smith' maxlength='32'>"
    "<label>Email Address <small>(optional)</small></label>"
    "<input name='email' type='email' placeholder='jane@example.com' maxlength='64'>"
    "<hr>"
    "<label>SSID &mdash; Select or Enter <small>(for OTA updates)</small></label>";

static const char HTML_FORM_TAIL[] =
    "<label>Wi-Fi Password <small>(leave blank if open)</small></label>"
    "<input name='pass' type='password' maxlength='64'>"
    "<hr>"
    "<label>App Manifest URL</label>"
    "<input name='manifest' value='https://raw.githubusercontent.com/watsonlr/namebadge-apps/main/manifest.json' maxlength='128'>"
    "<button type='submit'>Save Settings</button>"
    "</form></body></html>";

static const char *HTML_OK =
    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Saved</title></head><body>"
    "<h2>Saved!</h2><p>The badge will now connect to your network. "
    "You may close this page.</p></body></html>";

/* ── Captive-portal redirect handler ────────────────────────────── */
/*
 * iOS, Android, Windows and macOS all probe a known URL after joining
 * an open network.  Because our DNS hijack returns 192.168.4.1 for any
 * hostname, those probes arrive here.  We 302-redirect them to our
 * setup page, which triggers the OS "Sign in to network" popup.
 */
static esp_err_t redirect_handler(httpd_req_t *req, httpd_err_code_t error)
{
    (void)error;
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", s_ap_url);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, "Redirecting to setup page", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* ── Helpers ─────────────────────────────────────────────────────── */

/** Percent-decode a URL-encoded string in-place. */
static void url_decode(char *buf, size_t buflen)
{
    char *src = buf;
    char *dst = buf;
    char tmp[3] = {0};
    size_t written = 0;

    while (*src && written + 1 < buflen) {
        if (*src == '%' && src[1] && src[2]) {
            tmp[0] = src[1]; tmp[1] = src[2];
            *dst++ = (char)strtol(tmp, NULL, 16);
            src += 3;
        } else if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else {
            *dst++ = *src++;
        }
        written++;
    }
    *dst = '\0';
}

/** Extract a field value from an x-www-form-urlencoded body. */
static bool form_field(const char *body, const char *key,
                        char *out, size_t outlen)
{
    size_t klen = strlen(key);
    const char *p = body;
    while (*p) {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            p += klen + 1;
            size_t i = 0;
            while (*p && *p != '&' && i + 1 < outlen) {
                out[i++] = *p++;
            }
            out[i] = '\0';
            url_decode(out, outlen);
            return true;
        }
        /* Skip to next field */
        while (*p && *p != '&') p++;
        if (*p == '&') p++;
    }
    return false;
}

/* ── HTTP handlers ───────────────────────────────────────────────── */

static esp_err_t get_root_handler(httpd_req_t *req)
{
    /* Only flag form-served when a real browser loads the page.
     * OS captive-portal auto-probes (iOS CaptiveNetworkSupport,
     * Android Dalvik, Windows NCSI, etc.) do NOT send a Mozilla
     * User-Agent string — so we ignore them here.
     *
     * httpd_req_get_hdr_value_str returns ESP_ERR_HTTPD_RESULT_TRUNC
     * when the UA is longer than the buffer (common — real UAs are
     * 100+ chars).  We accept that too: "Mozilla" is always the very
     * first word so a 64-byte truncated read still captures it.      */
    char ua[64] = {0};
    esp_err_t ua_err = httpd_req_get_hdr_value_str(req, "User-Agent",
                                                    ua, sizeof(ua));
    if ((ua_err == ESP_OK || ua_err == ESP_ERR_HTTPD_RESULT_TRUNC)
            && strstr(ua, "Mozilla") != NULL) {
        s_form_served = true;
    }

    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");

    /* ── Head (everything up to the SSID field) ── */
    httpd_resp_sendstr_chunk(req, HTML_FORM_HEAD);

    /* ── SSID field: datalist of discovered networks ── */
    if (s_scan_count > 0) {
        /* An <input list='nets'> lets the user pick from the dropdown
         * OR type any custom SSID — no JS required.                  */
        httpd_resp_sendstr_chunk(req,
            "<input name='ssid' list='nets' required maxlength='32'"
            " placeholder='Select or type SSID'>"
            "<datalist id='nets'>");

        for (uint16_t i = 0; i < s_scan_count; i++) {
            const char *ssid = (const char *)s_scan_aps[i].ssid;
            if (ssid[0] == '\0') continue;   /* skip hidden networks */

            /* Deduplicate (simple O(n²) — list is max 20 entries) */
            bool dup = false;
            for (uint16_t j = 0; j < i; j++) {
                if (strcmp(ssid, (const char *)s_scan_aps[j].ssid) == 0) {
                    dup = true;
                    break;
                }
            }
            if (dup) continue;

            /* Build "<option value='ESCAPED_SSID'>" safely */
            char opt[96];
            char *p   = opt;
            char *end = opt + sizeof(opt) - 1;
            const char *pre = "<option value='";
            memcpy(p, pre, strlen(pre));
            p += strlen(pre);
            html_esc(&p, end, ssid);
            if (p + 2 <= end) { *p++ = '\''; *p++ = '>'; }
            *p = '\0';
            httpd_resp_sendstr_chunk(req, opt);
        }

        httpd_resp_sendstr_chunk(req, "</datalist>");
    } else {
        /* Fallback when scan yielded nothing */
        httpd_resp_sendstr_chunk(req,
            "<input name='ssid' required maxlength='32'"
            " placeholder='Network name'>");
    }

    /* ── Tail (password, manifest, button, closing tags) ── */
    httpd_resp_sendstr_chunk(req, HTML_FORM_TAIL);
    httpd_resp_sendstr_chunk(req, NULL);   /* end chunked transfer */
    return ESP_OK;
}

static esp_err_t post_save_handler(httpd_req_t *req)
{
    /* Buffer: fields up to 32+64+32+64+128 = 320 raw chars, but URL-encoding
     * can triple special characters (e.g. a password full of symbols).
     * 1024 B gives comfortable headroom.                                  */
    char body[1024] = {0};
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No body");
        return ESP_FAIL;
    }
    body[received] = '\0';

    char ssid[33]      = {0};
    char pass[65]      = {0};
    char nick[33]      = {0};
    char email[65]     = {0};
    char manifest[129] = {0};

    if (!form_field(body, "ssid", ssid, sizeof(ssid)) || ssid[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing SSID");
        return ESP_FAIL;
    }
    form_field(body, "pass",     pass,     sizeof(pass));     /* optional */
    form_field(body, "nick",     nick,     sizeof(nick));     /* optional */
    form_field(body, "email",    email,    sizeof(email));    /* optional */
    form_field(body, "manifest", manifest, sizeof(manifest)); /* optional */

    /* Write to the dedicated user_data NVS partition (top of flash). */
    nvs_handle_t h;
    esp_err_t err = nvs_open_from_partition(WIFI_CONFIG_NVS_PARTITION,
                                            WIFI_CONFIG_NVS_NAMESPACE,
                                            NVS_READWRITE, &h);
    if (err == ESP_OK) {
        nvs_set_str(h, WIFI_CONFIG_NVS_KEY_SSID,     ssid);
        nvs_set_str(h, WIFI_CONFIG_NVS_KEY_PASS,     pass);
        nvs_set_str(h, WIFI_CONFIG_NVS_KEY_NICK,     nick);
        nvs_set_str(h, WIFI_CONFIG_NVS_KEY_EMAIL,    email);
        nvs_set_str(h, WIFI_CONFIG_NVS_KEY_MANIFEST, manifest);
        nvs_commit(h);
        nvs_close(h);
        ESP_LOGI(TAG, "Saved to user_data: nick='%s' email='%s' ssid='%s'",
                 nick, email, ssid);
        s_done = true;
    } else {
        ESP_LOGE(TAG, "user_data NVS write failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "NVS error");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, HTML_OK, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* ── HTTP server ─────────────────────────────────────────────────── */

static const httpd_uri_t uri_root = {
    .uri     = "/",
    .method  = HTTP_GET,
    .handler = get_root_handler,
};

static const httpd_uri_t uri_save = {
    .uri     = "/save",
    .method  = HTTP_POST,
    .handler = post_save_handler,
};

static bool start_http_server(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port      = 80;
    cfg.max_open_sockets = 4;
    cfg.lru_purge_enable = true;
    /* Header length is set via CONFIG_HTTPD_MAX_REQ_HDR_LEN in sdkconfig. */

    if (httpd_start(&s_server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server start failed");
        return false;
    }
    httpd_register_uri_handler(s_server, &uri_root);
    httpd_register_uri_handler(s_server, &uri_save);
    /* Redirect everything else → triggers OS captive-portal popup */
    httpd_register_err_handler(s_server, HTTPD_404_NOT_FOUND, redirect_handler);
    ESP_LOGI(TAG, "HTTP server started on port 80");
    return true;
}

/* ── SoftAP ──────────────────────────────────────────────────────── */

static bool start_softap(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    s_ap_netif = esp_netif_create_default_wifi_ap();
    /* ── Derive unique AP IP from factory eFuse MAC ────────────────
     * Formula: 192.168.(mac[4] % 240).(mac[5] % 240)
     * Last octet clamped to ≥ 1 so it is never a network address.  */
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);

    /* Unique SSID: BYUI_NameBadge_XX */
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "%s_%02X", AP_SSID_BASE, mac[5]);

    /* Unique IP: 192.168.(mac[4]%240).(mac[5]%240) */
    s_ap_ip[2] = mac[4] % 240;
    s_ap_ip[3] = mac[5] % 240;
    if (s_ap_ip[3] == 0) s_ap_ip[3] = 1;   /* avoid .X.0 network addr */
    snprintf(s_ap_url, sizeof(s_ap_url), "http://%d.%d.%d.%d/",
             s_ap_ip[0], s_ap_ip[1], s_ap_ip[2], s_ap_ip[3]);
    ESP_LOGI(TAG, "AP: SSID=\"%s\"  IP=%d.%d.%d.%d  (MAC ...%02X:%02X)",
             s_ap_ssid, s_ap_ip[0], s_ap_ip[1], s_ap_ip[2], s_ap_ip[3],
             mac[4], mac[5]);

    /* Apply custom IP to the netif before wifi starts. */
    ESP_ERROR_CHECK(esp_netif_dhcps_stop(s_ap_netif));
    esp_netif_ip_info_t ip_info = {0};
    IP4_ADDR(&ip_info.ip,      s_ap_ip[0], s_ap_ip[1], s_ap_ip[2], s_ap_ip[3]);
    IP4_ADDR(&ip_info.gw,      s_ap_ip[0], s_ap_ip[1], s_ap_ip[2], s_ap_ip[3]);
    IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);
    ESP_ERROR_CHECK(esp_netif_set_ip_info(s_ap_netif, &ip_info));
    ESP_ERROR_CHECK(esp_netif_dhcps_start(s_ap_netif));
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    /* Register AP connect/disconnect events */
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT,
        WIFI_EVENT_AP_STACONNECTED,   ap_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT,
        WIFI_EVENT_AP_STADISCONNECTED, ap_event_handler, NULL));

    wifi_config_t ap_cfg = {
        .ap = {
            .ssid_len        = 0,   /* set after strncpy below */
            .channel         = AP_CHANNEL,
            .authmode        = WIFI_AUTH_OPEN,
            .max_connection  = AP_MAX_CONN,
        },
    };

    strncpy((char *)ap_cfg.ap.ssid, s_ap_ssid, sizeof(ap_cfg.ap.ssid) - 1);
    ap_cfg.ap.ssid_len = (uint8_t)strlen(s_ap_ssid);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "SoftAP started: SSID=\"%s\"  IP=%d.%d.%d.%d",
             s_ap_ssid, s_ap_ip[0], s_ap_ip[1], s_ap_ip[2], s_ap_ip[3]);

    /* Scan nearby networks now so the form can show a dropdown.
     * Temporarily uses APSTA mode; reverts to AP before returning. */
    scan_nearby_networks();

    return true;
}

/* ── Public API ──────────────────────────────────────────────────── */

bool wifi_config_start(void)
{
    nvs_flash_init(); /* system NVS — harmless if already initialised */
    /* Init the dedicated user_data partition (idempotent). */
    esp_err_t ue = nvs_flash_init_partition(WIFI_CONFIG_NVS_PARTITION);
    if (ue == ESP_ERR_NVS_NO_FREE_PAGES || ue == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase_partition(WIFI_CONFIG_NVS_PARTITION);
        nvs_flash_init_partition(WIFI_CONFIG_NVS_PARTITION);
    }
    s_done        = false;
    s_sta_joined  = false;
    s_form_served = false;

    if (!start_softap())      return false;
    if (!start_http_server()) return false;
    start_dns_server();
    return true;
}

void wifi_config_stop(void)
{
    stop_dns_server();
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }
    esp_wifi_stop();
    esp_wifi_deinit();
    if (s_ap_netif) {
        esp_netif_destroy(s_ap_netif);
        s_ap_netif = NULL;
    }
}

bool wifi_config_done(void)
{
    return s_done;
}

bool wifi_config_sta_joined(void)
{
    return s_sta_joined;
}

bool wifi_config_form_served(void)
{
    return s_form_served;
}



const char *wifi_config_ssid(void)
{
    return s_ap_ssid;
}

const char *wifi_config_url(void)
{
    return s_ap_url;
}

bool wifi_config_is_configured(void)
{
    /* Initialise the user_data partition (idempotent). */
    esp_err_t ue = nvs_flash_init_partition(WIFI_CONFIG_NVS_PARTITION);
    if (ue == ESP_ERR_NVS_NO_FREE_PAGES || ue == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase_partition(WIFI_CONFIG_NVS_PARTITION);
        nvs_flash_init_partition(WIFI_CONFIG_NVS_PARTITION);
    }

    nvs_handle_t h;
    if (nvs_open_from_partition(WIFI_CONFIG_NVS_PARTITION,
                                WIFI_CONFIG_NVS_NAMESPACE,
                                NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    char nick[33] = {0};
    size_t len = sizeof(nick);
    nvs_get_str(h, WIFI_CONFIG_NVS_KEY_NICK, nick, &len);
    nvs_close(h);
    return nick[0] != '\0';
}

void wifi_config_get_nick(char *out, size_t outlen)
{
    if (!out || outlen == 0) return;
    out[0] = '\0';

    nvs_handle_t h;
    if (nvs_open_from_partition(WIFI_CONFIG_NVS_PARTITION,
                                WIFI_CONFIG_NVS_NAMESPACE,
                                NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    size_t len = outlen;
    nvs_get_str(h, WIFI_CONFIG_NVS_KEY_NICK, out, &len);
    nvs_close(h);
}
