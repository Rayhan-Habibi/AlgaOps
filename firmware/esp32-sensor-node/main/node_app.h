/*
 * node_app.h — AlgaOps sensor node application (measurement only: the node
 * never drives a relay, an actuator or a PBR setpoint).
 */

#ifndef NODE_APP_H
#define NODE_APP_H

/** Start sensors, network and the node tasks.  Requires NVS, esp_netif and
 *  the default event loop (see app_main.c). */
void node_app_start(void);

#endif /* NODE_APP_H */
