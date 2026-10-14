# ESP32-S31 radio co-processor firmware: ESP-IDF's Wi-Fi and Bluetooth
# controller on hart 1 for NetBSD on hart 0.  Build with build.sh, which
# applies idf-patches/ and packs the ELF into the image NetBSD loads.
cmake_minimum_required(VERSION 3.22)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(espcp)
