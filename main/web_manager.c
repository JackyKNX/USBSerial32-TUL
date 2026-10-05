#include "web_manager.h"
#include "mqtt_manager.h"
#include "bridge_stats.h"

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_err.h"
#include "esp_timer.h"

#include "nvs.h"
#include "nvs_flash.h"

#define WEB_PORT                    80
#define WIFI_RETRY_DELAY_MS        3000
#define WIFI_RETRY_MAX_DELAY_MS    30000
#define WIFI_AP_FALLBACK_MS        30000

#define AP_PASSWORD                "tulsetup"

#define OTA_USER                   "admin"
#define OTA_PASSWORD               "tul"

#define WIFI_CONNECTED_BIT         BIT0

static httpd_handle_t s_server = NULL;
static EventGroupHandle_t s_wifi_event_group = NULL;

static bool s_ap_mode = false;
static bool s_wifi_started = false;
static volatile bool s_wifi_reconfiguring = false;
static bool s_sta_has_ip = false;
static int32_t s_last_disconnect_reason = -1;
static uint32_t s_disconnect_count = 0;

static char s_wifi_ssid[64] = "";
static char s_wifi_password[64] = "";

/* -------------------------------------------------------------------------- */
/* Helpers                                                                    */
/* -------------------------------------------------------------------------- */

static void copy_string(char *dst, size_t dst_size, const char *src)
{
    if (dst_size == 0)
        return;

    strncpy(dst, src, dst_size - 1);
    dst[dst_size - 1] = '\0';
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';

    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;

    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;

    return -1;
}

/*
 * Decode application/x-www-form-urlencoded:
 *
 *   '+'  -> space
 *   %XX  -> byte
 */
static void url_decode(
    const char *src,
    char *dst,
    size_t dst_size)
{
    size_t di = 0;

    if (dst_size == 0)
        return;

    while (*src && di < dst_size - 1) {

        if (*src == '+') {
            dst[di++] = ' ';
            src++;
            continue;
        }

        if (*src == '%' &&
            src[1] &&
            src[2]) {

            int h = hex_value(src[1]);
            int l = hex_value(src[2]);

            if (h >= 0 && l >= 0) {
                dst[di++] = (char)((h << 4) | l);
                src += 3;
                continue;
            }
        }

        dst[di++] = *src++;
    }

    dst[di] = '\0';
}

/* -------------------------------------------------------------------------- */
/* NVS                                                                        */
/* -------------------------------------------------------------------------- */

static void wifi_load_config(void)
{
    nvs_handle_t nvs;

    if (nvs_open("wifi", NVS_READONLY, &nvs) != ESP_OK)
        return;

    size_t len = sizeof(s_wifi_ssid);

    if (nvs_get_str(nvs, "ssid", s_wifi_ssid, &len) != ESP_OK)
        s_wifi_ssid[0] = '\0';

    len = sizeof(s_wifi_password);

    if (nvs_get_str(nvs, "password", s_wifi_password, &len) != ESP_OK)
        s_wifi_password[0] = '\0';

    nvs_close(nvs);
}

static esp_err_t wifi_save_config(
    const char *ssid,
    const char *password)
{
    nvs_handle_t nvs;

    esp_err_t err = nvs_open(
        "wifi",
        NVS_READWRITE,
        &nvs);

    if (err != ESP_OK)
        return err;

    err = nvs_set_str(nvs, "ssid", ssid);

    if (err == ESP_OK)
        err = nvs_set_str(nvs, "password", password);

    if (err == ESP_OK)
        err = nvs_commit(nvs);

    nvs_close(nvs);

    if (err != ESP_OK)
        return err;

    copy_string(
        s_wifi_ssid,
        sizeof(s_wifi_ssid),
        ssid);

    copy_string(
        s_wifi_password,
        sizeof(s_wifi_password),
        password);

    return ESP_OK;
}

/* -------------------------------------------------------------------------- */
/* WiFi                                                                       */
/* -------------------------------------------------------------------------- */

static void wifi_event_handler(
    void *arg,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT) {

        if (event_id == WIFI_EVENT_STA_START) {
            if (!s_ap_mode &&
                s_wifi_started &&
                !s_wifi_reconfiguring) {
                (void)esp_wifi_connect();
            }
        }

        else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
            wifi_event_sta_disconnected_t *event = event_data;

            s_sta_has_ip = false;
            s_disconnect_count++;

            if (event)
                s_last_disconnect_reason = event->reason;
            else
                s_last_disconnect_reason = -1;

            xEventGroupClearBits(
                s_wifi_event_group,
                WIFI_CONNECTED_BIT);

            /*
             * Reconnect is deliberately owned by wifi_worker_task().
             * Calling esp_wifi_connect() from the system event callback
             * can race with esp_wifi_start()/esp_wifi_stop()/set_mode().
             */
        }
    }

    else if (event_base == IP_EVENT &&
             event_id == IP_EVENT_STA_GOT_IP) {

        s_sta_has_ip = true;

        xEventGroupSetBits(
            s_wifi_event_group,
            WIFI_CONNECTED_BIT);
    }

    else if (event_base == IP_EVENT &&
             event_id == IP_EVENT_STA_LOST_IP) {

        s_sta_has_ip = false;

        xEventGroupClearBits(
            s_wifi_event_group,
            WIFI_CONNECTED_BIT);
    }
}

static esp_err_t wifi_start_ap(void)
{
    wifi_config_t cfg = {0};
    uint8_t mac[6] = {0};

    esp_err_t err = esp_wifi_get_mac(WIFI_IF_AP, mac);
    if (err != ESP_OK)
        return err;

    snprintf(
        (char *)cfg.ap.ssid,
        sizeof(cfg.ap.ssid),
        "TUL-%02X%02X%02X",
        mac[3], mac[4], mac[5]);

    copy_string(
        (char *)cfg.ap.password,
        sizeof(cfg.ap.password),
        AP_PASSWORD);

    cfg.ap.ssid_len = strlen((char *)cfg.ap.ssid);
    cfg.ap.channel = 1;
    cfg.ap.max_connection = 4;
    cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;

    /* AP is a recovery interface. Keep STA alive in parallel. */
    s_wifi_reconfiguring = true;
    s_ap_mode = true;

    err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err != ESP_OK) {
        s_ap_mode = false;
        s_wifi_reconfiguring = false;
        return err;
    }

    err = esp_wifi_set_config(WIFI_IF_AP, &cfg);
    if (err != ESP_OK) {
        s_ap_mode = false;
        s_wifi_reconfiguring = false;
        return err;
    }

    if (!s_wifi_started) {
        err = esp_wifi_start();
        if (err != ESP_OK) {
            s_wifi_reconfiguring = false;
            return err;
        }
        s_wifi_started = true;
    }

    s_wifi_reconfiguring = false;
    return ESP_OK;
}

static esp_err_t wifi_start_sta(void)
{
    if (s_wifi_ssid[0] == '\0')
        return ESP_ERR_INVALID_STATE;

    wifi_config_t cfg = {0};

    copy_string(
        (char *)cfg.sta.ssid,
        sizeof(cfg.sta.ssid),
        s_wifi_ssid);

    copy_string(
        (char *)cfg.sta.password,
        sizeof(cfg.sta.password),
        s_wifi_password);

    cfg.sta.scan_method = WIFI_FAST_SCAN;
    cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
    cfg.sta.pmf_cfg.capable = true;
    cfg.sta.pmf_cfg.required = false;

    s_wifi_reconfiguring = true;
    s_ap_mode = false;
    s_sta_has_ip = false;

    xEventGroupClearBits(
        s_wifi_event_group,
        WIFI_CONNECTED_BIT);

    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        s_wifi_reconfiguring = false;
        return err;
    }

    err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if (err != ESP_OK) {
        s_wifi_reconfiguring = false;
        return err;
    }

    err = esp_wifi_set_ps(WIFI_PS_NONE);
    if (err != ESP_OK) {
        s_wifi_reconfiguring = false;
        return err;
    }

    if (!s_wifi_started) {
        err = esp_wifi_start();
        if (err != ESP_OK) {
            s_wifi_reconfiguring = false;
            return err;
        }
        s_wifi_started = true;
    }

    s_wifi_reconfiguring = false;

    /* Connection is asynchronous. wifi_worker_task owns retries. */
    return ESP_OK;
}

static void wifi_worker_task(void *arg)
{
    (void)arg;

    uint32_t retry_delay = WIFI_RETRY_DELAY_MS;
    TickType_t ap_deadline = 0;

    while (1) {

        if (s_wifi_ssid[0] == '\0') {
            /* No configured STA: AP is the only recovery path. */
            if (!s_ap_mode)
                (void)wifi_start_ap();

            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        if (s_sta_has_ip) {
            retry_delay = WIFI_RETRY_DELAY_MS;
            ap_deadline = 0;
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        /* If WiFi is not started, start it before attempting STA connect. */
        if (!s_wifi_started) {
            s_wifi_reconfiguring = true;

            esp_err_t start_err = esp_wifi_start();

            if (start_err == ESP_OK) {
                s_wifi_started = true;
            } else {
                s_wifi_reconfiguring = false;
                vTaskDelay(pdMS_TO_TICKS(retry_delay));
                continue;
            }

            s_wifi_reconfiguring = false;
        }

        /* If we are in recovery AP mode, keep trying STA in the background. */
        if (s_ap_mode && ap_deadline == 0)
            ap_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(WIFI_AP_FALLBACK_MS);

        esp_err_t err = esp_wifi_connect();

        if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
            /* Driver rejected the request; retry with back-off. */
            vTaskDelay(pdMS_TO_TICKS(retry_delay));
        } else {
            /*
             * The driver is now connecting asynchronously.  The event
             * handler owns the state transition when GOT_IP arrives.
             */
            vTaskDelay(pdMS_TO_TICKS(retry_delay));

            if (retry_delay < WIFI_RETRY_MAX_DELAY_MS)
                retry_delay *= 2;

            if (retry_delay > WIFI_RETRY_MAX_DELAY_MS)
                retry_delay = WIFI_RETRY_MAX_DELAY_MS;
        }

        if (s_sta_has_ip) {
            retry_delay = WIFI_RETRY_DELAY_MS;
            ap_deadline = 0;

            /* AP is only a recovery interface. Drop it after STA is back. */
            if (s_ap_mode) {
                (void)wifi_start_sta();
            }

            continue;
        }

        if (!s_sta_has_ip && !s_ap_mode) {
            /*
             * Do not make the device disappear from the network permanently
             * after one slow boot.  First wait for a full retry window, then
             * expose the setup AP while STA retries continue.
             */
            if (ap_deadline == 0)
                ap_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(WIFI_AP_FALLBACK_MS);

            if ((int32_t)(xTaskGetTickCount() - ap_deadline) >= 0) {
                (void)wifi_start_ap();
                ap_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(WIFI_AP_FALLBACK_MS);
            }
        }
    }
}

/* -------------------------------------------------------------------------- */
/* HTTP helpers                                                               */
/* -------------------------------------------------------------------------- */

static esp_err_t send_html(
    httpd_req_t *req,
    const char *html)
{
    httpd_resp_set_type(
        req,
        "text/html; charset=utf-8");

    return httpd_resp_send(
        req,
        html,
        HTTPD_RESP_USE_STRLEN);
}

static const char *current_ip(void)
{
    static char ip[32];

    ip[0] = '\0';

    const char *ifkey =
        s_ap_mode ? "WIFI_AP_DEF" : "WIFI_STA_DEF";

    esp_netif_t *netif =
        esp_netif_get_handle_from_ifkey(ifkey);

    if (!netif)
        return "0.0.0.0";

    esp_netif_ip_info_t info;

    if (esp_netif_get_ip_info(
            netif,
            &info) != ESP_OK)
        return "0.0.0.0";

    snprintf(
        ip,
        sizeof(ip),
        IPSTR,
        IP2STR(&info.ip));

    return ip;
}

/* -------------------------------------------------------------------------- */
/* Root                                                                       */
/* -------------------------------------------------------------------------- */


static void format_uptime(uint32_t seconds, char *out, size_t out_size)
{
    uint32_t days = seconds / 86400;
    seconds %= 86400;

    uint32_t hours = seconds / 3600;
    seconds %= 3600;

    uint32_t minutes = seconds / 60;
    uint32_t secs = seconds % 60;

    if (days > 0) {
        snprintf(out, out_size,
                 "%lu days, %luh, %lum, %lu sec",
                 (unsigned long)days,
                 (unsigned long)hours,
                 (unsigned long)minutes,
                 (unsigned long)secs);
    } else if (hours > 0) {
        snprintf(out, out_size,
                 "%luh, %lum, %lu sec",
                 (unsigned long)hours,
                 (unsigned long)minutes,
                 (unsigned long)secs);
    } else if (minutes > 0) {
        snprintf(out, out_size,
                 "%lum, %lu sec",
                 (unsigned long)minutes,
                 (unsigned long)secs);
    } else {
        snprintf(out, out_size,
                 "%lu sec",
                 (unsigned long)secs);
    }
}

static esp_err_t handle_root(httpd_req_t *req)
{
    char *html = malloc(14000);
    if (!html) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");

    const char *mode = s_ap_mode ? "Access Point" : "Station";
    const char *ssid = s_ap_mode ? "TUL setup AP" : s_wifi_ssid;
    const esp_partition_t *running = esp_ota_get_running_partition();
    uint32_t uptime = (uint32_t)(esp_timer_get_time() / 1000000ULL);
    char uptime_text[64];
    uint32_t free_heap = esp_get_free_heap_size();
    format_uptime(uptime, uptime_text, sizeof(uptime_text));

    uint32_t min_heap = esp_get_minimum_free_heap_size();
    uint8_t wifi_mac[6] = {0};
    char wifi_mac_text[18] = "00:00:00:00:00:00";
    esp_reset_reason_t reset_reason = esp_reset_reason();
    const char *reset_text = "Unknown";

    if (esp_wifi_get_mac(WIFI_IF_STA, wifi_mac) == ESP_OK) {
        snprintf(
            wifi_mac_text,
            sizeof(wifi_mac_text),
            "%02X:%02X:%02X:%02X:%02X:%02X",
            wifi_mac[0], wifi_mac[1], wifi_mac[2],
            wifi_mac[3], wifi_mac[4], wifi_mac[5]);
    }

    switch (reset_reason) {
        case ESP_RST_POWERON: reset_text = "Power-on"; break;
        case ESP_RST_SW: reset_text = "Software"; break;
        case ESP_RST_PANIC: reset_text = "Panic"; break;
        case ESP_RST_INT_WDT: reset_text = "Interrupt watchdog"; break;
        case ESP_RST_TASK_WDT: reset_text = "Task watchdog"; break;
        case ESP_RST_WDT: reset_text = "Watchdog"; break;
        case ESP_RST_BROWNOUT: reset_text = "Brownout"; break;
        case ESP_RST_DEEPSLEEP: reset_text = "Deep sleep"; break;
        default: break;
    }

    bool transceiver_ok = bridge_transceiver_ok();
    bool host_seen = bridge_host_seen();

    snprintf(html, 14000,
        "<!doctype html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>BUSWARE TUL</title><style>"
        "*{box-sizing:border-box}"
        "body{font-family:Arial,sans-serif;margin:0;padding:24px;background:#f4f4f4;color:#222}"
        ".container{max-width:1100px;margin:auto}"
        ".header{background:#fff;padding:22px;margin-bottom:20px;border-radius:12px;box-shadow:0 2px 8px #ccc}"
        ".header h1{margin:0 0 6px 0}.header p{margin:4px 0;color:#666}"
        ".grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:20px}"
        ".card{background:#fff;padding:20px;border-radius:12px;box-shadow:0 2px 8px #ccc;min-width:0}"
        ".card h2{margin:0 0 16px 0}"
        ".row{display:flex;justify-content:space-between;align-items:flex-start;gap:20px;padding:8px 0;border-bottom:1px solid #eee}"
        ".row:last-child{border-bottom:0}.label{color:#666}.value{font-weight:bold;text-align:right;word-break:break-word}"
        ".ok{color:green}.warn{color:#b06000}.danger{background:#d9534f;color:white}"
        ".button{display:inline-block;padding:10px 16px;margin-top:10px;border:0;border-radius:6px;text-decoration:none;cursor:pointer;background:#ddd}"
        "a{color:#06c}label{display:block;margin-top:12px}input{padding:9px;width:100%%}button{padding:10px 16px;margin-top:15px}"
        "@media(max-width:700px){.grid{grid-template-columns:1fr}body{padding:12px}.row{flex-direction:column;gap:3px}.value{text-align:left}}"
        "</style></head><body><div class='container'>"
        "<div class='header'><h1>BUSWARE TUL</h1><p><b>USBSerial32</b> &mdash; Web Manager</p>"
        "<p>Firmware version: <b>%s</b></p></div>"
        "<div class='grid'>"
        "<div class='card'><h2>Network</h2>"
        "<div class='row'><span class='label'>Mode</span><span class='value'>%s</span></div>"
        "<div class='row'><span class='label'>IP address</span><span class='value'>%s</span></div>"
        "<div class='row'><span class='label'>WiFi MAC</span><span class='value'>%s</span></div>"
        "<div class='row'><span class='label'>SSID</span><span class='value'>%s</span></div>"
        "<div class='row'><span class='label'>WiFi status</span><span class='value %s'>%s</span></div>"
        "<div class='row'><span class='label'>Disconnects</span><span class='value'>%lu</span></div>"
        "<div class='row'><span class='label'>Last disconnect reason</span><span class='value'>%ld</span></div></div>"
        "<div class='card'><h2>System Health</h2>"
        "<div class='row'><span class='label'>Firmware</span><span class='value'>%s</span></div>"
        "<div class='row'><span class='label'>Uptime</span><span class='value'>%s</span></div>"
        "<div class='row'><span class='label'>Free heap</span><span class='value'>%lu KB</span></div>"
        "<div class='row'><span class='label'>Minimum heap</span><span class='value'>%lu KB</span></div>"
        "<div class='row'><span class='label'>Reset reason</span><span class='value'>%s</span></div>"
        "<div class='row'><span class='label'>Running partition</span><span class='value'>%s</span></div></div>"
        "<div class='card'><h2>KNX Bridge</h2>"
        "<div class='row'><span class='label'>Transceiver</span><span class='value %s'>%s</span></div>"
        "<div class='row'><span class='label'>Host seen</span><span class='value'>%s</span></div>"
        "<div class='row'><span class='label'>KNX RX</span><span class='value'>%lu bytes</span></div>"
        "<div class='row'><span class='label'>KNX TX</span><span class='value'>%lu bytes</span></div>"
        "<div class='row'><span class='label'>USB RX</span><span class='value'>%lu bytes</span></div>"
        "<div class='row'><span class='label'>USB TX</span><span class='value'>%lu bytes</span></div></div>"
        "<div class='card'><h2>MQTT</h2>"
        "<div class='row'><span class='label'>Status</span><span class='value %s'>%s</span></div>"
        "<div class='row'><span class='label'>Broker</span><span class='value'>%s:%u</span></div>"
        "<div class='row'><span class='label'>Base topic</span><span class='value'>%s</span></div>"
        "<p><a href='/mqtt'>MQTT configuration</a></p></div>"
        "<div class='card'><h2>WiFi configuration</h2><form method='POST' action='/wifi/save'>"
        "<label>SSID</label><input name='ssid' maxlength='63' value='%s' required>"
        "<label>Password</label><input name='password' type='password' maxlength='63'>"
        "<button type='submit'>Save &amp; reboot</button></form></div>"
        "<div class='card'><h2>Firmware</h2><p>OTA firmware update:</p>"
        "<p><a class='button' href='/update'>Open OTA update</a></p></div>"
        "<div class='card'><h2>System</h2><p>Restart the ESP32 without changing configuration.</p>"
        "<form method='POST' action='/restart'><button class='button danger' type='submit'>Restart ESP32</button></form></div>"
        "<div class='card'><h2>Serial Monitor</h2><p>Live USB &harr; KNX traffic monitor.</p>" \
        "<p><a class='button' href='/serial'>Open Serial Monitor</a></p></div>" \
        "<div class='card'><h2>About this project</h2>"
        "<div class='row'><span class='label'>Firmware version</span><span class='value'>%s</span></div>"
        "<div class='row'><span class='label'>Hardware</span><span class='value'>BUSWARE TUL ESP32-C3</span></div>"
        "<div class='row'><span class='label'>Based on</span><span class='value'><a href='https://github.com/tostmann/USBSerial32' target='_blank'>USBSerial32</a></span></div>"
        "<div class='row'><span class='label'>TUL project</span><span class='value'><a href='https://github.com/JackyKNX/USBSerial32-TUL' target='_blank'>USBSerial32-TUL</a></span></div>"
        "<p></p></div>"
        "<div class='card'><h2>API</h2><p><a href='/api/status'>System status JSON</a></p></div>"
        "</div></div></body></html>",
        BW_VERSION, mode,current_ip(),wifi_mac_text,ssid,
        s_sta_has_ip ? "ok" : "warn",
        s_sta_has_ip ? "Connected" : (s_ap_mode ? "Recovery AP" : "Connecting"),
        (unsigned long)s_disconnect_count,
        (long)s_last_disconnect_reason,
        BW_VERSION,uptime_text,
        (unsigned long)(free_heap/1024UL),(unsigned long)(min_heap/1024UL),reset_text,
        running ? running->label : "",transceiver_ok?"ok":"warn",transceiver_ok?"OK":"Not OK",
        host_seen?"Yes":"No",(unsigned long)bridge_knx_rx_bytes(),(unsigned long)bridge_knx_tx_bytes(),
        (unsigned long)bridge_usb_rx_bytes(),(unsigned long)bridge_usb_tx_bytes(),
        mqtt_manager_connected()?"ok":"warn",mqtt_manager_connected()?"Connected":(mqtt_manager_enabled()?"Disconnected":"Disabled"),
        mqtt_manager_host()[0]?mqtt_manager_host():"not configured",(unsigned)mqtt_manager_port(),mqtt_manager_topic(),s_wifi_ssid,BW_VERSION);

    esp_err_t err = send_html(req, html); free(html); return err;
}

/* -------------------------------------------------------------------------- */
/* Status API                                                                 */
/* -------------------------------------------------------------------------- */

static esp_err_t handle_status(httpd_req_t *req)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    uint32_t uptime = (uint32_t)(esp_timer_get_time()/1000000ULL);
    uint32_t free_heap = esp_get_free_heap_size();
    uint32_t min_heap = esp_get_minimum_free_heap_size();
    esp_reset_reason_t reset_reason = esp_reset_reason();
    const char *reset_text = "Unknown";
    switch (reset_reason) {
        case ESP_RST_POWERON: reset_text="Power-on"; break;
        case ESP_RST_SW: reset_text="Software"; break;
        case ESP_RST_PANIC: reset_text="Panic"; break;
        case ESP_RST_INT_WDT: reset_text="Interrupt watchdog"; break;
        case ESP_RST_TASK_WDT: reset_text="Task watchdog"; break;
        case ESP_RST_WDT: reset_text="Watchdog"; break;
        case ESP_RST_BROWNOUT: reset_text="Brownout"; break;
        case ESP_RST_DEEPSLEEP: reset_text="Deep sleep"; break;
        default: break;
    }
    char json[1536];
    snprintf(json,sizeof(json),
        "{\"mode\":\"%s\",\"ip\":\"%s\",\"ssid\":\"%s\",\"wifi_connected\":%s,\"wifi_disconnects\":%lu,\"wifi_last_disconnect_reason\":%ld,"
        "\"mqtt_enabled\":%s,\"mqtt_connected\":%s,\"mqtt_host\":\"%s\","
        "\"mqtt_port\":%u,\"mqtt_topic\":\"%s\",\"firmware\":\"%s\","
        "\"uptime\":%lu,\"free_heap\":%lu,\"min_heap\":%lu,\"reset_reason\":\"%s\","
        "\"transceiver_ok\":%s,\"host_seen\":%s,\"knx_rx\":%lu,\"knx_tx\":%lu,"
        "\"usb_rx\":%lu,\"usb_tx\":%lu,\"running_partition\":\"%s\","
        "\"running_address\":\"0x%lx\",\"next_partition\":\"%s\"}",
        s_ap_mode?"AP":"STA",current_ip(),s_ap_mode?"":s_wifi_ssid,
        s_sta_has_ip?"true":"false",
        (unsigned long)s_disconnect_count,
        (long)s_last_disconnect_reason,
        mqtt_manager_enabled()?"true":"false",mqtt_manager_connected()?"true":"false",
        mqtt_manager_host(),(unsigned)mqtt_manager_port(),mqtt_manager_topic(),BW_VERSION,
        (unsigned long)uptime,(unsigned long)free_heap,(unsigned long)min_heap,reset_text,
        bridge_transceiver_ok()?"true":"false",bridge_host_seen()?"true":"false",
        (unsigned long)bridge_knx_rx_bytes(),(unsigned long)bridge_knx_tx_bytes(),
        (unsigned long)bridge_usb_rx_bytes(),(unsigned long)bridge_usb_tx_bytes(),
        running?running->label:"",running?(unsigned long)running->address:0UL,next?next->label:"");
    httpd_resp_set_type(req,"application/json");
    return httpd_resp_send(req,json,HTTPD_RESP_USE_STRLEN);
}

/* -------------------------------------------------------------------------- */
/* WiFi save                                                                  */
/* -------------------------------------------------------------------------- */

static esp_err_t handle_wifi_save(httpd_req_t *req)
{
    char body[512];

    int len = httpd_req_recv(
        req,
        body,
        sizeof(body) - 1);

    if (len <= 0)
        return ESP_FAIL;

    body[len] = '\0';

    char ssid_encoded[128] = "";
    char password_encoded[128] = "";

    char ssid[64] = "";
    char password[64] = "";

    char *p = strstr(body, "ssid=");

    if (p)
        sscanf(
            p + 5,
            "%127[^&]",
            ssid_encoded);

    p = strstr(body, "password=");

    if (p)
        sscanf(
            p + 9,
            "%127[^&]",
            password_encoded);

    url_decode(
        ssid_encoded,
        ssid,
        sizeof(ssid));

    url_decode(
        password_encoded,
        password,
        sizeof(password));

    if (ssid[0] == '\0') {

        return httpd_resp_send_err(
            req,
            HTTPD_400_BAD_REQUEST,
            "SSID missing");
    }

    if (wifi_save_config(
            ssid,
            password) != ESP_OK) {

        return httpd_resp_send_err(
            req,
            HTTPD_500_INTERNAL_SERVER_ERROR,
            "Cannot save WiFi configuration");
    }

    httpd_resp_set_type(
        req,
        "text/html; charset=utf-8");

    httpd_resp_sendstr(
        req,
        "<html><body>"
        "<h2>WiFi configuration saved.</h2>"
        "<p>Rebooting...</p>"
        "</body></html>");

    vTaskDelay(pdMS_TO_TICKS(1000));

    esp_restart();

    return ESP_OK;
}


/* -------------------------------------------------------------------------- */
/* Restart                                                                    */
/* -------------------------------------------------------------------------- */

static esp_err_t handle_restart(httpd_req_t *req)
{
    httpd_resp_set_type(req,"text/html; charset=utf-8");
    httpd_resp_sendstr(req,
        "<!doctype html><html><body>"
        "<h2>ESP32 restarting...</h2><p>Please wait a few seconds.</p>"
        "</body></html>");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

/* -------------------------------------------------------------------------- */
/* MQTT configuration                                                         */
/* -------------------------------------------------------------------------- */

static esp_err_t handle_mqtt_page(httpd_req_t *req)
{
    char html[5000];

    snprintf(
        html,
        sizeof(html),

        "<!doctype html>"
        "<html><head>"
        "<meta charset='utf-8'>"
        "<meta name='viewport' "
        "content='width=device-width,initial-scale=1'>"
        "<title>BUSWARE TUL - MQTT</title>"

        "<style>"
        "body{font-family:Arial,sans-serif;"
        "margin:30px;background:#f4f4f4;color:#222}"
        ".card{background:#fff;padding:20px;"
        "margin-bottom:20px;border-radius:10px;"
        "max-width:800px;box-shadow:0 2px 8px #ccc}"
        "h1{margin-top:0}"
        "label{display:block;margin-top:12px}"
        "input{padding:9px;width:100%%;"
        "box-sizing:border-box}"
        "button{margin-top:15px;padding:10px 18px}"
        ".ok{color:green}"
        ".warn{color:#b06000}"
        "a{color:#06c}"
        "</style>"

        "</head><body>"

        "<div class='card'>"
        "<h1>BUSWARE TUL</h1>"
        "<h2>MQTT configuration</h2>"

        "<p>Status: <b class='%s'>%s</b></p>"

        "<form method='POST' action='/mqtt/save'>"

        "<label>"
        "<input type='checkbox' name='enabled' value='1' %s>"
        " Enable MQTT"
        "</label>"

        "<label>Broker IP / hostname</label>"
        "<input name='host' maxlength='127' "
        "value='%s'>"

        "<label>Port</label>"
        "<input name='port' type='number' "
        "min='1' max='65535' value='%u'>"

        "<label>Username</label>"
        "<input name='username' maxlength='63' "
        "value='%s'>"

        "<label>Password</label>"
        "<input name='password' type='password' "
        "maxlength='127'>"

        "<p>Password is not displayed. "
        "Leave empty to keep the stored password.</p>"

        "<label>Base topic</label>"
        "<input name='topic' maxlength='127' "
        "value='%s'>"

        "<button type='submit'>Save &amp; reboot</button>"

        "</form>"

        "<p><a href='/'>&larr; Back</a></p>"

        "</div>"

        "</body></html>",

        mqtt_manager_connected()
            ? "ok"
            : "warn",

        mqtt_manager_connected()
            ? "Connected"
            : (mqtt_manager_enabled()
                ? "Disconnected"
                : "Disabled"),

        mqtt_manager_enabled()
            ? "checked"
            : "",

        mqtt_manager_host(),

        (unsigned)mqtt_manager_port(),

        mqtt_manager_username(),

        mqtt_manager_topic());

    return send_html(req, html);
}

static esp_err_t handle_mqtt_save(httpd_req_t *req)
{
    char body[1024];

    int len = httpd_req_recv(
        req,
        body,
        sizeof(body) - 1);

    if (len <= 0)
        return ESP_FAIL;

    body[len] = '\0';

    char host_encoded[256] = "";
    char port_encoded[32] = "";
    char username_encoded[128] = "";
    char password_encoded[256] = "";
    char topic_encoded[256] = "";

    char host[128] = "";
    char username[64] = "";
    char password[128] = "";
    char topic[128] = "";

    bool enabled =
        strstr(body, "enabled=1") != NULL;

    char *p;

    p = strstr(body, "host=");
    if (p)
        sscanf(
            p + 5,
            "%255[^&]",
            host_encoded);

    p = strstr(body, "port=");
    if (p)
        sscanf(
            p + 5,
            "%31[^&]",
            port_encoded);

    p = strstr(body, "username=");
    if (p)
        sscanf(
            p + 9,
            "%127[^&]",
            username_encoded);

    p = strstr(body, "password=");
    if (p)
        sscanf(
            p + 9,
            "%255[^&]",
            password_encoded);

    p = strstr(body, "topic=");
    if (p)
        sscanf(
            p + 6,
            "%255[^&]",
            topic_encoded);

    url_decode(
        host_encoded,
        host,
        sizeof(host));

    url_decode(
        username_encoded,
        username,
        sizeof(username));

    url_decode(
        password_encoded,
        password,
        sizeof(password));

    url_decode(
        topic_encoded,
        topic,
        sizeof(topic));

    uint16_t port = 1883;

    if (port_encoded[0] != '\0') {

        long value =
            strtol(port_encoded, NULL, 10);

        if (value < 1 || value > 65535) {

            return httpd_resp_send_err(
                req,
                HTTPD_400_BAD_REQUEST,
                "Invalid MQTT port");
        }

        port = (uint16_t)value;
    }

    if (enabled && host[0] == '\0') {

        return httpd_resp_send_err(
            req,
            HTTPD_400_BAD_REQUEST,
            "MQTT broker cannot be empty");
    }

    if (topic[0] == '\0')
        copy_string(
            topic,
            sizeof(topic),
            "busware/TUL");

    /*
     * mqtt_manager_save_config() keeps the existing password
     * when password is an empty string.
     */
    esp_err_t err =
        mqtt_manager_save_config(
            enabled,
            host,
            port,
            username,
            password,
            topic);

    if (err != ESP_OK) {

        return httpd_resp_send_err(
            req,
            HTTPD_500_INTERNAL_SERVER_ERROR,
            "Cannot save MQTT configuration");
    }

    httpd_resp_set_type(
        req,
        "text/html; charset=utf-8");

    httpd_resp_sendstr(
        req,
        "<html><body>"
        "<h2>MQTT configuration saved.</h2>"
        "<p>Rebooting...</p>"
        "</body></html>");

    vTaskDelay(
        pdMS_TO_TICKS(1000));

    esp_restart();

    return ESP_OK;
}


/* -------------------------------------------------------------------------- */
/* OTA authentication                                                         */
/* -------------------------------------------------------------------------- */

static bool ota_authorized(httpd_req_t *req)
{
    char auth[256];

    if (httpd_req_get_hdr_value_str(
            req,
            "Authorization",
            auth,
            sizeof(auth)) != ESP_OK) {

        httpd_resp_set_status(
            req,
            "401 Unauthorized");

        httpd_resp_set_hdr(
            req,
            "WWW-Authenticate",
            "Basic realm=\"TUL OTA\"");

        httpd_resp_send(
            req,
            NULL,
            0);

        return false;
    }

    /*
     * admin:tul
     *
     * Base64 = YWRtaW46dHVs
     *
     * Development/test credentials for now.
     */

    if (strcmp(
            auth,
            "Basic YWRtaW46dHVs") == 0)
        return true;

    httpd_resp_set_status(
        req,
        "401 Unauthorized");

    httpd_resp_set_hdr(
        req,
        "WWW-Authenticate",
        "Basic realm=\"TUL OTA\"");

    httpd_resp_send(
        req,
        NULL,
        0);

    return false;
}

/* -------------------------------------------------------------------------- */
/* OTA page                                                                   */
/* -------------------------------------------------------------------------- */

static esp_err_t handle_update_page(httpd_req_t *req)
{
    if (!ota_authorized(req))
        return ESP_OK;

    const char *html =
        "<!doctype html>"
        "<html><head>"
        "<meta charset='utf-8'>"
        "<meta name='viewport' "
        "content='width=device-width,initial-scale=1'>"
        "<title>TUL OTA</title>"
        "</head><body>"

        "<h1>BUSWARE TUL firmware update</h1>"

"<p>Select firmware image:</p>"

"<input type='file' id='firmware' accept='.bin' required>"
"<br>"
"<button type='button' onclick='uploadFirmware()'>"
"Upload firmware"
"</button>"

"<p id='status'></p>"

"<script>"
"async function uploadFirmware(){"
" const input=document.getElementById('firmware');"
" const status=document.getElementById('status');"
" if(!input.files.length){"
"   status.textContent='Please select a firmware file.';"
"   return;"
" }"
" const file=input.files[0];"
" status.textContent='Uploading '+file.name+' ('+file.size+' bytes)...';"
" try{"
"   const response=await fetch('/update',{"
"     method:'POST',"
"     headers:{'Content-Type':'application/octet-stream'},"
"     body:file"
"   });"
"   const text=await response.text();"
"   document.body.innerHTML=text;"
" }catch(e){"
"   status.textContent='Upload failed: '+e;"
" }"
"}"
"</script>"

        "<p><a href='/'>Back</a></p>"

        "</body></html>";

    return send_html(req, html);
}

/* -------------------------------------------------------------------------- */
/* OTA upload                                                                 */
/* -------------------------------------------------------------------------- */

static esp_err_t handle_update_upload(httpd_req_t *req)
{
    if (!ota_authorized(req))
        return ESP_OK;

    const esp_partition_t *partition =
        esp_ota_get_next_update_partition(NULL);

    if (!partition) {

        return httpd_resp_send_err(
            req,
            HTTPD_500_INTERNAL_SERVER_ERROR,
            "No OTA partition");
    }

    esp_ota_handle_t ota;

    esp_err_t err =
        esp_ota_begin(
            partition,
            OTA_WITH_SEQUENTIAL_WRITES,
            &ota);

    if (err != ESP_OK) {

        return httpd_resp_send_err(
            req,
            HTTPD_500_INTERNAL_SERVER_ERROR,
            "OTA begin failed");
    }

    uint8_t buffer[4096];

    int remaining = req->content_len;

    while (remaining > 0) {

        int to_read = remaining;

        if (to_read > (int)sizeof(buffer))
            to_read = sizeof(buffer);

        int received =
            httpd_req_recv(
                req,
                (char *)buffer,
                to_read);

        if (received <= 0) {

            esp_ota_abort(ota);

            httpd_resp_send_err(
                req,
                HTTPD_500_INTERNAL_SERVER_ERROR,
                "Upload failed");

            return ESP_FAIL;
        }

        err = esp_ota_write(
            ota,
            buffer,
            received);

        if (err != ESP_OK) {

            esp_ota_abort(ota);

            httpd_resp_send_err(
                req,
                HTTPD_500_INTERNAL_SERVER_ERROR,
                "OTA write failed");

            return err;
        }

        remaining -= received;
    }

    err = esp_ota_end(ota);

    if (err != ESP_OK) {

        httpd_resp_send_err(
            req,
            HTTPD_500_INTERNAL_SERVER_ERROR,
            "OTA validation failed");

        return err;
    }

    err = esp_ota_set_boot_partition(
        partition);

    if (err != ESP_OK) {

        httpd_resp_send_err(
            req,
            HTTPD_500_INTERNAL_SERVER_ERROR,
            "Cannot set boot partition");

        return err;
    }

    httpd_resp_set_type(
        req,
        "text/html; charset=utf-8");

    httpd_resp_sendstr(
        req,
        "<html><body>"
        "<h1>OTA successful</h1>"
        "<p>Rebooting...</p>"
        "</body></html>");

    vTaskDelay(pdMS_TO_TICKS(1000));

    esp_restart();

    return ESP_OK;
}

/* -------------------------------------------------------------------------- */
/* Serial Monitor                                                             */
/* -------------------------------------------------------------------------- */

#define SERIAL_LOG_SIZE   8192
#define SERIAL_LINE_SIZE  160

static char s_serial_log[SERIAL_LOG_SIZE];
static size_t s_serial_head = 0;
static size_t s_serial_tail = 0;

static char s_serial_rx_line[SERIAL_LINE_SIZE];
static char s_serial_tx_line[SERIAL_LINE_SIZE];

static uint16_t s_serial_rx_len = 0;
static uint16_t s_serial_tx_len = 0;

static uint32_t s_serial_rx_last = 0;
static uint32_t s_serial_tx_last = 0;

static portMUX_TYPE s_serial_lock = portMUX_INITIALIZER_UNLOCKED;

static void serial_log_put_locked(char c)
{
    s_serial_log[s_serial_head] = c;
    s_serial_head = (s_serial_head + 1) % SERIAL_LOG_SIZE;

    if (s_serial_head == s_serial_tail)
        s_serial_tail = (s_serial_tail + 1) % SERIAL_LOG_SIZE;
}

static void serial_log_line_locked(
    char direction,
    const char *line,
    uint16_t len)
{
    if (len == 0)
        return;

    for (uint16_t i = 0; i < len; i++)
        serial_log_put_locked(line[i]);

    serial_log_put_locked('\n');
}

static void serial_flush_line_locked(char direction)
{
    char *line;
    uint16_t *len;

    if (direction == 'R') {
        line = s_serial_rx_line;
        len = &s_serial_rx_len;
    } else {
        line = s_serial_tx_line;
        len = &s_serial_tx_len;
    }

    serial_log_line_locked(direction, line, *len);

    *len = 0;
    line[0] = '\0';
}

void web_manager_log_byte(char direction, uint8_t value)
{
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000ULL);

    portENTER_CRITICAL(&s_serial_lock);

    char *line;
    uint16_t *len;
    uint32_t *last;

    if (direction == 'R') {
        line = s_serial_rx_line;
        len = &s_serial_rx_len;
        last = &s_serial_rx_last;
    } else {
        line = s_serial_tx_line;
        len = &s_serial_tx_len;
        last = &s_serial_tx_last;
    }

    /*
     * Same framing principle as the PlatformIO monitor:
     * a new frame starts after 5 ms of silence.
     */
    if (*len > 0 && (uint32_t)(now - *last) > 5)
        serial_flush_line_locked(direction);

    if (*len == 0) {
        line[0] = direction;
        line[1] = 'X';
        line[2] = ' ';
        *len = 3;
    }

    /*
     * Keep each displayed frame bounded.  A long continuous stream
     * is split into multiple monitor lines instead of growing memory.
     */
    if (*len + 3 >= SERIAL_LINE_SIZE) {
        serial_flush_line_locked(direction);
        line[0] = direction;
        line[1] = 'X';
        line[2] = ' ';
        *len = 3;
    }

    static const char hex[] = "0123456789ABCDEF";

    line[(*len)++] = hex[(value >> 4) & 0x0F];
    line[(*len)++] = hex[value & 0x0F];
    line[(*len)++] = ' ';
    line[*len] = '\0';

    *last = now;

    portEXIT_CRITICAL(&s_serial_lock);
}

static esp_err_t handle_serial_api(httpd_req_t *req)
{
    char *snapshot = malloc(SERIAL_LOG_SIZE);
    char *json = malloc(SERIAL_LOG_SIZE + 512);

    if (!snapshot || !json) {
        free(snapshot);
        free(json);

        return httpd_resp_send_err(
            req,
            HTTPD_500_INTERNAL_SERVER_ERROR,
            "Out of memory");
    }

    size_t snapshot_len = 0;

    portENTER_CRITICAL(&s_serial_lock);

    serial_flush_line_locked('R');
    serial_flush_line_locked('T');

    size_t pos = s_serial_tail;

    while (pos != s_serial_head && snapshot_len < SERIAL_LOG_SIZE - 1) {
        snapshot[snapshot_len++] = s_serial_log[pos];
        pos = (pos + 1) % SERIAL_LOG_SIZE;
    }

    snapshot[snapshot_len] = '\0';

    /*
     * Everything currently buffered has been delivered to this client.
     */
    s_serial_tail = s_serial_head;

    portEXIT_CRITICAL(&s_serial_lock);

    size_t out = 0;

    json[out++] = '[';
    bool first = true;

    size_t start = 0;

    while (start < snapshot_len) {
        size_t end = start;

        while (end < snapshot_len && snapshot[end] != '\n')
            end++;

        if (end > start) {
            if (!first)
                json[out++] = ',';

            first = false;

            json[out++] = '"';

            for (size_t i = start; i < end; i++) {
                char c = snapshot[i];

                if (c == '"' || c == '\\') {
                    if (out + 2 >= SERIAL_LOG_SIZE + 512)
                        break;

                    json[out++] = '\\';
                    json[out++] = c;
                } else {
                    if (out + 1 >= SERIAL_LOG_SIZE + 512)
                        break;

                    json[out++] = c;
                }
            }

            json[out++] = '"';
        }

        start = (end < snapshot_len) ? end + 1 : snapshot_len;
    }

    json[out++] = ']';
    json[out] = '\0';

    free(snapshot);

    httpd_resp_set_type(
        req,
        "application/json");

    esp_err_t err = httpd_resp_send(
        req,
        json,
        HTTPD_RESP_USE_STRLEN);

    free(json);

    return err;
}

static esp_err_t handle_serial_page(httpd_req_t *req)
{
    const char *html =
        "<!doctype html>"
        "<html><head>"
        "<meta charset='utf-8'>"
        "<meta name='viewport' "
        "content='width=device-width,initial-scale=1'>"
        "<title>BUSWARE TUL - Serial Monitor</title>"

        "<style>"
        "body{font-family:Arial,sans-serif;"
        "margin:0;padding:24px;background:#f4f4f4;color:#222}"
        ".container{max-width:1100px;margin:auto}"
        ".card{background:#fff;padding:20px;border-radius:12px;"
        "box-shadow:0 2px 8px #ccc}"
        "h1{margin-top:0}"
        "button{padding:9px 15px;margin-right:8px;"
        "border:0;border-radius:6px;cursor:pointer}"
        "a{color:#06c}"
        ".toolbar{display:flex;align-items:center;gap:15px;"
        "margin-bottom:12px;flex-wrap:wrap}"
        "#log{background:#111;color:#ddd;padding:12px;"
        "height:560px;overflow:auto;font-family:monospace;"
        "font-size:13px;line-height:1.45;border-radius:6px;"
        "white-space:pre-wrap;word-break:break-all}"
        "</style>"

        "</head><body>"

        "<div class='container'>"
        "<div class='card'>"

        "<h1>BUSWARE TUL - Serial Monitor</h1>"

        "<div class='toolbar'>"
        "<button onclick='clearLog()'>Clear</button>"
        "<label>"
        "<input type='checkbox' id='scroll' checked>"
        " Auto scroll"
        "</label>"
        "<span id='state'>Running</span>"
        "</div>"

        "<pre id='log'></pre>"

        "<p><a href='/'>&larr; Back to Web Manager</a></p>"

        "</div>"
        "</div>"

        "<script>"
        "let running=true;"

        "async function poll(){"
        " if(!running)return;"
        " try{"
        "  const r=await fetch('/api/serial',{cache:'no-store'});"
        "  const a=await r.json();"
        "  const l=document.getElementById('log');"
        "  a.forEach(x=>{"
        "   l.textContent+=new Date().toLocaleTimeString()+' '+x+'\\n';"
        "  });"
        "  if(document.getElementById('scroll').checked)"
        "   l.scrollTop=l.scrollHeight;"
        " }catch(e){}"
        " setTimeout(poll,250);"
        "}"

        "function clearLog(){"
        " document.getElementById('log').textContent='';"
        "}"

        "window.addEventListener('beforeunload',()=>{running=false;});"
        "poll();"
        "</script>"

        "</body></html>";

    return send_html(req, html);
}

/* -------------------------------------------------------------------------- */
/* HTTP server                                                                */
/* -------------------------------------------------------------------------- */

static void start_http_server(void)
{
    httpd_config_t config =
        HTTPD_DEFAULT_CONFIG();

    config.server_port = WEB_PORT;
    config.stack_size = 8192;
    config.max_uri_handlers = 14;

    if (httpd_start(
            &s_server,
            &config) != ESP_OK)
        return;

    httpd_uri_t root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = handle_root,
        .user_ctx = NULL
    };

    httpd_uri_t serial_page = {
        .uri = "/serial",
        .method = HTTP_GET,
        .handler = handle_serial_page,
        .user_ctx = NULL
    };

    httpd_uri_t serial_api = {
        .uri = "/api/serial",
        .method = HTTP_GET,
        .handler = handle_serial_api,
        .user_ctx = NULL
    };

    httpd_uri_t mqtt_page = {
        .uri = "/mqtt",
        .method = HTTP_GET,
        .handler = handle_mqtt_page,
        .user_ctx = NULL
    };

    httpd_uri_t mqtt_save = {
        .uri = "/mqtt/save",
        .method = HTTP_POST,
        .handler = handle_mqtt_save,
        .user_ctx = NULL
    };


    httpd_uri_t status = {
        .uri = "/api/status",
        .method = HTTP_GET,
        .handler = handle_status,
        .user_ctx = NULL
    };

    httpd_uri_t restart = {
        .uri = "/restart",
        .method = HTTP_POST,
        .handler = handle_restart,
        .user_ctx = NULL
    };

    httpd_uri_t wifi_save = {
        .uri = "/wifi/save",
        .method = HTTP_POST,
        .handler = handle_wifi_save,
        .user_ctx = NULL
    };

    httpd_uri_t update_page = {
        .uri = "/update",
        .method = HTTP_GET,
        .handler = handle_update_page,
        .user_ctx = NULL
    };

    httpd_uri_t update_upload = {
        .uri = "/update",
        .method = HTTP_POST,
        .handler = handle_update_upload,
        .user_ctx = NULL
    };

    httpd_register_uri_handler(
        s_server,
        &root);

    httpd_register_uri_handler(
        s_server,
        &serial_page);

    httpd_register_uri_handler(
        s_server,
        &serial_api);

    httpd_register_uri_handler(
        s_server,
        &status);

    httpd_register_uri_handler(
        s_server,
        &restart);

    httpd_register_uri_handler(
        s_server,
        &wifi_save);

    httpd_register_uri_handler(
        s_server,
        &mqtt_page);

    httpd_register_uri_handler(
        s_server,
        &mqtt_save);

    httpd_register_uri_handler(
        s_server,
        &update_page);

    httpd_register_uri_handler(
        s_server,
        &update_upload);
}

/* -------------------------------------------------------------------------- */
/* Public                                                                     */
/* -------------------------------------------------------------------------- */

const char *web_manager_ip(void)
{
    return current_ip();
}

const char *web_manager_ssid(void)
{
    return s_ap_mode ? "" : s_wifi_ssid;
}

void web_manager_start(void)
{
    esp_err_t err = nvs_flash_init();

    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {

        ESP_ERROR_CHECK(
            nvs_flash_erase());

        ESP_ERROR_CHECK(
            nvs_flash_init());
    }

    ESP_ERROR_CHECK(
        esp_netif_init());

    ESP_ERROR_CHECK(
        esp_event_loop_create_default());

    s_wifi_event_group =
        xEventGroupCreate();

    if (!s_wifi_event_group)
        return;

    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    ESP_ERROR_CHECK(
        esp_event_handler_register(
            WIFI_EVENT,
            ESP_EVENT_ANY_ID,
            &wifi_event_handler,
            NULL));

    ESP_ERROR_CHECK(
        esp_event_handler_register(
            IP_EVENT,
            IP_EVENT_STA_GOT_IP,
            &wifi_event_handler,
            NULL));

    ESP_ERROR_CHECK(
        esp_event_handler_register(
            IP_EVENT,
            IP_EVENT_STA_LOST_IP,
            &wifi_event_handler,
            NULL));

    wifi_init_config_t wifi_cfg =
        WIFI_INIT_CONFIG_DEFAULT();

    ESP_ERROR_CHECK(
        esp_wifi_init(&wifi_cfg));

    wifi_load_config();

    /*
     * Start the normal STA path first.  A slow DHCP/authentication sequence
     * must not make the device permanently fall back to AP mode: the worker
     * keeps retrying STA in the background and AP is only a recovery path.
     */
    if (wifi_start_sta() != ESP_OK)
        (void)wifi_start_ap();

    start_http_server();

    xTaskCreate(
        wifi_worker_task,
        "wifi_worker",
        4096,
        NULL,
        4,
        NULL);
}