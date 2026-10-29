/*
 * wifi_link.h — Wi-Fi station with exponential reconnect back-off
 * (1 s doubling to 60 s).  WPA2 or better is required.
 */

#ifndef WIFI_LINK_H
#define WIFI_LINK_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/** Requires esp_netif_init() and the default event loop.  Non-blocking. */
esp_err_t wifi_link_start(const char *ssid, const char *password);
bool      wifi_link_connected(void);
uint32_t  wifi_link_reconnects(void);

/** RSSI of the current access point; false when not connected. */
bool      wifi_link_rssi(int *rssi_dbm);

#endif /* WIFI_LINK_H */
