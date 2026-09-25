#pragma once

#include <stdint.h>

void web_manager_start(void);

const char *web_manager_ip(void);
const char *web_manager_ssid(void);

/*
 * Copy one bridge byte into the Web Serial Monitor diagnostic buffer.
 * This never writes to the KNX/USB transport itself.
 */
void web_manager_log_byte(char direction, uint8_t value);
