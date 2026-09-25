#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

void mqtt_manager_start(void);

bool mqtt_manager_enabled(void);
bool mqtt_manager_connected(void);

const char *mqtt_manager_host(void);
uint16_t mqtt_manager_port(void);
const char *mqtt_manager_username(void);
const char *mqtt_manager_topic(void);

esp_err_t mqtt_manager_save_config(
    bool enabled,
    const char *host,
    uint16_t port,
    const char *username,
    const char *password,
    const char *topic);