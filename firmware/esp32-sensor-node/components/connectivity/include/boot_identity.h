/*
 * boot_identity.h — per-boot identity of the node.
 *
 * boot_id is a fresh 32-bit random value for every boot (esp_random(), a true
 * RNG once the RF subsystem runs, mixed with a persistent boot counter), so
 * the gateway can tell a restart from replayed or duplicated windows even if
 * NVS is erased.  The reset reason is reported in the status message.
 */

#ifndef BOOT_IDENTITY_H
#define BOOT_IDENTITY_H

#include <stdint.h>

#include "esp_err.h"

typedef struct {
    uint32_t    boot_id;
    uint32_t    boot_count;      /* persistent in NVS ("algaops" namespace) */
    const char *reset_reason;    /* static string                           */
} boot_identity_t;

/** Call after esp_wifi_start() so that esp_random() is a true RNG. */
esp_err_t boot_identity_init(boot_identity_t *out);

#endif /* BOOT_IDENTITY_H */
