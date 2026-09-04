cmake_minimum_required(VERSION 3.22)

project(edge-telemetry-gateway
        VERSION 0.1.0
        DESCRIPTION "Multi-threaded CAN telemetry gateway with lock-free fan-out"
        LANGUAGES CXX)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(ETG_BUILD_TESTS "Build tests" ON)
option(ETG_WERROR      "Treat warnings as errors" OFF)
set(ETG_SANITIZER "none" CACHE STRING "none|asan|tsan|ubsan")
set_property(CACHE ETG_SANITIZER PROPERTY STRINGS none asan tsan ubsan)

include(cmake/CompilerWarnings.cmake)
include(cmake/Sanitizers.cmake)

add_subdirectory(src)

if(ETG_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

message(STATUS "etg: build=${CMAKE_BUILD_TYPE} sanitizer=${ETG_SANITIZER} tests=${ETG_BUILD_TESTS} werror=${ETG_WERROR}")
