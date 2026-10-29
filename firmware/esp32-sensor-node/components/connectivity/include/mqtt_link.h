/*
 * mqtt_link.h — publish-only MQTT client (ESP-MQTT, MQTT 3.1.1).
 *
 * The node subscribes to nothing and accepts no commands: AlgaOps is
 * read-only towards the plant.  Telemetry is published with QoS 1 and only
 * removed from the node buffer after the broker's PUBACK; the retained
 * status topic carries "online" and the last will "offline".
 */

#ifndef MQTT_LINK_H
#define MQTT_LINK_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    const char *uri;           /* mqtt://host:1883 or mqtts://host:8883        */
    const char *client_id;     /* the node id                                  */
    const char *username;      /* NULL / "" when the broker allows anonymous   */
    const char *password;
    const char *ca_pem;        /* CA certificate for mqtts://, NULL otherwise  */
    const char *lwt_topic;     /* retained "offline" status                     */
    const char *lwt_payload;
} mqtt_link_config_t;

esp_err_t mqtt_link_start(const mqtt_link_config_t *cfg);
bool      mqtt_link_connected(void);

/** Incremented on every (re)connection: lets the caller re-announce "online". */
uint32_t  mqtt_link_epoch(void);
uint32_t  mqtt_link_disconnects(void);

/** Publish while connected; returns the message id (>= 0) or -1. */
int       mqtt_link_publish(const char *topic, const char *payload, int len, int qos, bool retain);

/** Wait for the PUBACK of @p msg_id (QoS 1). */
bool      mqtt_link_wait_ack(int msg_id, uint32_t timeout_ms);

#endif /* MQTT_LINK_H */
