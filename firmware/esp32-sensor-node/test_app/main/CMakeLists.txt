idf_component_register(SRCS "test_app_main.c" "test_adxl345_selftest.c" "test_ds18b20_read.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES unity adxl345 ds18b20 node_core driver esp_timer)
