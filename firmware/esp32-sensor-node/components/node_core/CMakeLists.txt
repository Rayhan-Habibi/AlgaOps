# Host-testable node logic: no ESP-IDF headers in these sources (see host_test/).
idf_component_register(SRCS "ring_buffer.c" "sample_window.c" "sensor_health.c"
                            "sequence_counter.c" "telemetry_payload.c"
                       INCLUDE_DIRS "include"
                       REQUIRES algaops_features)
