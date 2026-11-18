# The ESP32-S3 gateway (docs/PLAN.md 4.2): ESP-IDF around the protocol core
# and application in ../core and ../app, which build and test on the Mac too.
cmake_minimum_required(VERSION 3.22)

# WiFi and MQTT credentials stay out of git: sdkconfig.secrets (see README.md).
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.secrets")
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;sdkconfig.secrets")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(tado_gateway)
