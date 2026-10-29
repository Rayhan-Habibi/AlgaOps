idf_component_register(SRCS "boot_identity.c" "mqtt_link.c" "time_sync.c" "wifi_link.c"
                       INCLUDE_DIRS "include"
                       REQUIRES node_core
                       PRIV_REQUIRES esp_wifi esp_netif esp_event esp_timer mqtt nvs_flash lwip)
