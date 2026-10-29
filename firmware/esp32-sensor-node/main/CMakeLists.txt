idf_component_register(SRCS "app_main.c" "node_app.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES adxl345 ds18b20 current_sensor connectivity node_core
                                     driver esp_app_format esp_event esp_netif esp_timer
                                     nvs_flash)
