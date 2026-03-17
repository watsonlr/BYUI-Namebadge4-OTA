cmake_minimum_required(VERSION 3.16)

# Declare target before including ESP-IDF so it cannot be overridden by stale
# cmake cache entries or IDE extensions that default to a different chip.
set(IDF_TARGET "esp32s3")

# Use the custom partition table from the project root
set(PARTITION_CSV_PATH "${CMAKE_SOURCE_DIR}/partitions.csv")

set(EXTRA_COMPONENT_DIRS "buttons" "loader_menu" "splash_screen" "display" "wifi_config" "portal_mode" "ota_manager" "leds")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(ebadge_app)
