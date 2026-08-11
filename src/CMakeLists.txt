# This file was automatically generated for projects
# without default 'CMakeLists.txt' file.

FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.*)

# PlatformIO 将 Arduino 核心注册为 framework-arduinoespressif32 组件。
idf_component_register(SRCS ${app_sources} REQUIRES framework-arduinoespressif32 bt esp-sr)
