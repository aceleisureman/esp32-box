cmake_minimum_required(VERSION 3.16.0)
# Override the cached project name from PlatformIO (which derives it from the
# directory path and may contain non-ASCII characters).
set(CMAKE_PROJECT_NAME "ESP32_BT_Amp" CACHE STRING "Project name" FORCE)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(ESP32_BT_Amp)
