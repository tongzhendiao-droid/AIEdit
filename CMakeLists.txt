cmake_minimum_required(VERSION 3.21)

project(cppEdit
    VERSION 0.1.0
    DESCRIPTION "A fast, AI-assisted viewer for text and binary files"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

option(CPPEDIT_BUILD_TESTS "Build the unit tests" ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets)
qt_standard_project_setup()

# Shared warning flags, linked privately into every target we own.
add_library(cppedit_warnings INTERFACE)
if(MSVC)
    target_compile_options(cppedit_warnings INTERFACE /W4 /permissive- /utf-8)
else()
    target_compile_options(cppedit_warnings INTERFACE -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion)
endif()

add_subdirectory(src)

if(CPPEDIT_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
