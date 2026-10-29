/*
 * time_sync.h — SNTP client (esp_sntp) against the gateway's local NTP server
 * (chrony on the Raspberry Pi; no internet needed).
 *
 * Window times are taken on the monotonic esp_timer clock; this module keeps
 * the offset to Unix time measured at each SNTP update (time_base_t of
 * telemetry_payload.h).  Until the first update the node reports
 * "time_synced": false and the gateway flags its windows.
 */

#ifndef TIME_SYNC_H
#define TIME_SYNC_H

#include <stdbool.h>

#include "telemetry_payload.h"

void time_sync_start(const char *server);
void time_sync_get_base(time_base_t *out);
bool time_sync_is_synced(void);

#endif /* TIME_SYNC_H */
