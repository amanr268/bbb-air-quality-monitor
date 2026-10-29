cmake_minimum_required(VERSION 3.16)
project(aqm VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)
add_compile_definitions(_GNU_SOURCE)

enable_testing()

add_subdirectory(common)
add_subdirectory(apps)
add_subdirectory(tests)
