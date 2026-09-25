#pragma once

#include <stdint.h>
#include <stdbool.h>

uint32_t bridge_usb_rx_bytes(void);
uint32_t bridge_usb_tx_bytes(void);
uint32_t bridge_knx_rx_bytes(void);
uint32_t bridge_knx_tx_bytes(void);
uint32_t bridge_traffic(void);

uint32_t bridge_probe_attempts(void);

bool bridge_transceiver_ok(void);
bool bridge_host_seen(void);