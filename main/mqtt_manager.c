#include "mqtt_manager.h"
#include "bridge_stats.h"
#include "web_manager.h"

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_timer.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_mac.h"

#include "mqtt_client.h"

#include "nvs.h"
#include "nvs_flash.h"

#include "esp_partition.h"
#include "esp_ota_ops.h"

#ifndef BW_VERSION
#define BW_VERSION "0.0.0"
#endif

#define MQTT_STATUS_INTERVAL_MS 60000

static esp_mqtt_client_handle_t s_client = NULL;

static bool s_enabled = false;
static bool s_connected = false;

static char s_host[128] = "";
static uint16_t s_port = 1883;
static char s_username[64] = "";
static char s_password[64] = "";
static char s_topic[128] = "busware/TUL";

static char s_availability_topic[160];
static char s_status_topic[160];
static char s_event_topic[160];
static char s_error_topic[160];
static char s_knx_bytes_topic[160];

static void copy_string(
    char *dst,
    size_t size,
    const char *src)
{
    if (!dst || size == 0)
        return;

    strncpy(dst, src ? src : "", size - 1);
    dst[size - 1] = '\0';
}

static void build_topics(void)
{
    snprintf(
        s_availability_topic,
        sizeof(s_availability_topic),
        "%s/availability",
        s_topic);

    snprintf(
        s_status_topic,
        sizeof(s_status_topic),
        "%s/status",
        s_topic);

    snprintf(
        s_event_topic,
        sizeof(s_event_topic),
        "%s/event",
        s_topic);

    snprintf(
        s_error_topic,
        sizeof(s_error_topic),
        "%s/error",
        s_topic);

    snprintf(
        s_knx_bytes_topic,
        sizeof(s_knx_bytes_topic),
        "%s/knx/bytes",
        s_topic);
}

static void load_config(void)
{
    nvs_handle_t nvs;

    if (nvs_open("mqtt", NVS_READONLY, &nvs) != ESP_OK)
        return;

    uint8_t enabled = 0;
    size_t len;

    if (nvs_get_u8(nvs, "enabled", &enabled) == ESP_OK)
        s_enabled = enabled != 0;

    len = sizeof(s_host);
    nvs_get_str(nvs, "host", s_host, &len);

    uint16_t port = 1883;
    if (nvs_get_u16(nvs, "port", &port) == ESP_OK)
        s_port = port;

    len = sizeof(s_username);
    nvs_get_str(nvs, "username", s_username, &len);

    len = sizeof(s_password);
    nvs_get_str(nvs, "password", s_password, &len);

    len = sizeof(s_topic);
    if (nvs_get_str(nvs, "topic", s_topic, &len) != ESP_OK)
        copy_string(s_topic, sizeof(s_topic), "busware/TUL");

    nvs_close(nvs);

    build_topics();
}

esp_err_t mqtt_manager_save_config(
    bool enabled,
    const char *host,
    uint16_t port,
    const char *username,
    const char *password,
    const char *topic)
{
    nvs_handle_t nvs;

    esp_err_t err = nvs_open(
        "mqtt",
        NVS_READWRITE,
        &nvs);

    if (err != ESP_OK)
        return err;

    err = nvs_set_u8(
        nvs,
        "enabled",
        enabled ? 1 : 0);

    if (err == ESP_OK)
        err = nvs_set_str(
            nvs,
            "host",
            host ? host : "");

    if (err == ESP_OK)
        err = nvs_set_u16(
            nvs,
            "port",
            port);

    if (err == ESP_OK)
        err = nvs_set_str(
            nvs,
            "username",
            username ? username : "");

    if (err == ESP_OK &&
        password &&
        password[0] != '\0') {

        err = nvs_set_str(
            nvs,
            "password",
            password);
    }

    if (err == ESP_OK)
        err = nvs_set_str(
            nvs,
            "topic",
            (topic && topic[0])
                ? topic
                : "busware/TUL");

    if (err == ESP_OK)
        err = nvs_commit(nvs);

    nvs_close(nvs);

    return err;
}

static void publish(
    const char *topic,
    const char *payload,
    int retain)
{
    if (!s_client || !s_connected)
        return;

    esp_mqtt_client_publish(
        s_client,
        topic,
        payload,
        0,
        0,
        retain);
}

static void publish_status(void)
{
    char json[1400];

    wifi_ap_record_t ap = {0};

    int rssi = 0;

    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK)
        rssi = ap.rssi;

    const esp_partition_t *running =
        esp_ota_get_running_partition();

    snprintf(
        json,
        sizeof(json),
        "{"
        "\"firmware\":\"%s\","
        "\"uptime\":%lu,"
        "\"ip\":\"%s\","
        "\"ssid\":\"%s\","
        "\"rssi\":%d,"
        "\"knx_rx\":%lu,"
        "\"knx_tx\":%lu,"
        "\"usb_rx\":%lu,"
        "\"usb_tx\":%lu,"
        "\"heap\":%lu,"
        "\"min_heap\":%lu,"
        "\"reset_reason\":%d,"
        "\"partition\":\"%s\","
        "\"transceiver_ok\":%s,"
        "\"host_seen\":%s"
        "}",
        BW_VERSION,
        (unsigned long)(
            esp_timer_get_time() / 1000000ULL),
        web_manager_ip(),
        web_manager_ssid(),
        rssi,
        (unsigned long)bridge_knx_rx_bytes(),
        (unsigned long)bridge_knx_tx_bytes(),
        (unsigned long)bridge_usb_rx_bytes(),
        (unsigned long)bridge_usb_tx_bytes(),
        (unsigned long)esp_get_free_heap_size(),
        (unsigned long)esp_get_minimum_free_heap_size(),
        (int)esp_reset_reason(),
        running ? running->label : "",
        bridge_transceiver_ok() ? "true" : "false",
        bridge_host_seen() ? "true" : "false");

    publish(
        s_status_topic,
        json,
        1);

    char bytes[32];

    snprintf(
        bytes,
        sizeof(bytes),
        "%lu",
        (unsigned long)(
            bridge_knx_rx_bytes() +
            bridge_knx_tx_bytes()));

    publish(
        s_knx_bytes_topic,
        bytes,
        1);
}

static void mqtt_event_handler(
    void *handler_args,
    esp_event_base_t base,
    int32_t event_id,
    void *event_data)
{
    (void)handler_args;
    (void)base;

    esp_mqtt_event_handle_t event =
        (esp_mqtt_event_handle_t)event_data;

    switch (event_id) {

    case MQTT_EVENT_CONNECTED:

        s_connected = true;

        publish(
            s_availability_topic,
            "online",
            1);

        publish(
            s_event_topic,
            "connected",
            0);

        publish_status();

        break;

    case MQTT_EVENT_DISCONNECTED:

        s_connected = false;

        break;

    case MQTT_EVENT_ERROR:

        if (event &&
            event->error_handle) {

            char error[64];

            snprintf(
                error,
                sizeof(error),
                "mqtt_error_type=%d",
                event->error_handle->error_type);

            publish(
                s_error_topic,
                error,
                0);
        }

        break;

    default:
        break;
    }

    return;
}

static void mqtt_status_task(void *arg)
{
    (void)arg;

    while (1) {

        vTaskDelay(
            pdMS_TO_TICKS(
                MQTT_STATUS_INTERVAL_MS));

        if (s_connected)
            publish_status();
    }
}

void mqtt_manager_start(void)
{
    load_config();

    if (!s_enabled ||
        s_host[0] == '\0') {

        return;
    }

    static char uri[192];

    snprintf(
        uri,
        sizeof(uri),
        "mqtt://%s:%u",
        s_host,
        (unsigned)s_port);

    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = uri,

        .credentials.username =
            s_username[0]
                ? s_username
                : NULL,

        .credentials.authentication.password =
            s_password[0]
                ? s_password
                : NULL,

        .session.last_will.topic =
            s_availability_topic,

        .session.last_will.msg =
            "offline",

        .session.last_will.qos = 1,

        .session.last_will.retain = 1,

        .session.keepalive = 60,

        .network.reconnect_timeout_ms = 5000,

        .buffer.size = 1024,

        .buffer.out_size = 1024,
    };

    s_client =
        esp_mqtt_client_init(&cfg);

    if (!s_client)
        return;

    esp_mqtt_client_register_event(
        s_client,
        MQTT_EVENT_ANY,
        mqtt_event_handler,
        NULL);

    if (esp_mqtt_client_start(s_client) != ESP_OK) {
        s_client = NULL;
        return;
    }

    xTaskCreate(
        mqtt_status_task,
        "mqtt_status",
        4096,
        NULL,
        4,
        NULL);
}

bool mqtt_manager_enabled(void)
{
    return s_enabled;
}

bool mqtt_manager_connected(void)
{
    return s_connected;
}

const char *mqtt_manager_host(void)
{
    return s_host;
}

uint16_t mqtt_manager_port(void)
{
    return s_port;
}

const char *mqtt_manager_username(void)
{
    return s_username;
}

const char *mqtt_manager_topic(void)
{
    return s_topic;
}