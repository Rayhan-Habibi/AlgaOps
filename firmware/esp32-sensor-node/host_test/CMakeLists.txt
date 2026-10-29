# Host tests of the firmware logic that does not touch hardware: register and
# scratchpad decoding, self-test limits, window assembly, health tracking,
# buffering, sequence numbers and the JSON payload.  No ESP-IDF needed:
#
#   cmake -S firmware/esp32-sensor-node/host_test -B firmware/esp32-sensor-node/host_test/build
#   cmake --build firmware/esp32-sensor-node/host_test/build
#   ctest --test-dir firmware/esp32-sensor-node/host_test/build --output-on-failure
#
# The drivers' bus code (I2C, 1-Wire, ADC), Wi-Fi and MQTT need the target;
# test_app/ holds the on-target (hardware-in-the-loop) Unity tests.

cmake_minimum_required(VERSION 3.16)
project(algaops_node_host_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
option(NODE_HOST_WERROR "Treat warnings as errors" OFF)

set(FW   ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(CORE ${FW}/../../algaops-assurance)

if(MSVC)
    add_compile_options(/W4)
else()
    add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Wformat=2)
    if(NODE_HOST_WERROR)
        add_compile_options(-Werror)
    endif()
endif()

add_library(node_logic STATIC
    ${FW}/components/adxl345/adxl345_convert.c
    ${FW}/components/ds18b20/ds18b20_convert.c
    ${FW}/components/node_core/ring_buffer.c
    ${FW}/components/node_core/sample_window.c
    ${FW}/components/node_core/sensor_health.c
    ${FW}/components/node_core/sequence_counter.c
    ${FW}/components/node_core/telemetry_payload.c
    ${CORE}/src/features.c)
target_include_directories(node_logic PUBLIC
    ${FW}/components/adxl345/include
    ${FW}/components/ds18b20/include
    ${FW}/components/node_core/include
    ${CORE}/include)
if(NOT WIN32)
    target_link_libraries(node_logic PUBLIC m)
endif()

enable_testing()
foreach(name adxl345_convert ds18b20_convert ring_buffer sample_window sensor_health
             sequence_counter telemetry_payload)
    add_executable(test_${name} test_${name}.c)
    target_link_libraries(test_${name} PRIVATE node_logic)
    target_include_directories(test_${name} PRIVATE ${CORE}/tests)
    add_test(NAME ${name} COMMAND test_${name})
endforeach()
