# AlgaOps Assurance ESP32 sensor node (ESP-IDF >= 5.2).
#   idf.py set-target esp32 && idf.py menuconfig && idf.py build flash monitor
cmake_minimum_required(VERSION 3.16)
set(PROJECT_VER "2.0.0")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(algaops_sensor_node)
