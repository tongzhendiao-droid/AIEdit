cmake_minimum_required(VERSION 3.21)

project(AIEdit
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

option(AIEDIT_BUILD_TESTS "Build the unit tests" ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets Concurrent Network)
qt_standard_project_setup()

# Optional: better charset detection, and syntax highlighting (the engine Kate uses).
find_package(uchardet CONFIG QUIET)
find_package(KF6SyntaxHighlighting 6.0 CONFIG QUIET)
message(STATUS "uchardet: ${uchardet_FOUND}, KF6SyntaxHighlighting: ${KF6SyntaxHighlighting_FOUND}")

# Shared warning flags, linked privately into every target we own.
add_library(aiedit_warnings INTERFACE)
if(MSVC)
    target_compile_options(aiedit_warnings INTERFACE /W4 /permissive- /utf-8)
else()
    target_compile_options(aiedit_warnings INTERFACE -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion)
endif()

add_subdirectory(src)

if(AIEDIT_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
