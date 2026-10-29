# On-target (hardware-in-the-loop) Unity tests for the sensor drivers.
# Flash to an ESP32 wired as in docs/hardware-assembly.md and run the tests
# from the serial menu:
#   idf.py -C firmware/esp32-sensor-node/test_app set-target esp32
#   idf.py -C firmware/esp32-sensor-node/test_app build flash monitor
cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../components")
set(COMPONENTS main)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(algaops_node_hil_tests)
