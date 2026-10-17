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

if(WIN32)
    enable_language(RC) # the icon and version of aiedit.exe
endif()

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets Concurrent Network)
qt_standard_project_setup()

# Optional: better charset detection, syntax highlighting (the engine Kate uses), and
# disassembling the code around differences between binaries (Capstone). On Windows,
# packaging/windows/build-deps.ps1 builds all three; add its prefix to CMAKE_PREFIX_PATH.
find_package(uchardet CONFIG QUIET)
find_package(KF6SyntaxHighlighting 6.0 CONFIG QUIET)
# Capstone by pkg-config where it has that (its .pc file points into include/capstone, while the
# code includes <capstone/capstone.h>, which only works where include/ is searched anyway), else
# by its header and library.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND AND NOT WIN32)
    pkg_check_modules(CAPSTONE QUIET IMPORTED_TARGET capstone)
endif()
if(CAPSTONE_FOUND)
    set(AIEDIT_CAPSTONE PkgConfig::CAPSTONE)
else()
    find_path(CAPSTONE_INCLUDE_DIR capstone/capstone.h)
    find_library(CAPSTONE_LIBRARY capstone)
    if(CAPSTONE_INCLUDE_DIR AND CAPSTONE_LIBRARY)
        add_library(aiedit_capstone UNKNOWN IMPORTED)
        set_target_properties(aiedit_capstone PROPERTIES
            IMPORTED_LOCATION "${CAPSTONE_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${CAPSTONE_INCLUDE_DIR}")
        set(AIEDIT_CAPSTONE aiedit_capstone)
        set(CAPSTONE_FOUND TRUE)
    endif()
endif()
message(STATUS "uchardet: ${uchardet_FOUND}, KF6SyntaxHighlighting: ${KF6SyntaxHighlighting_FOUND}, "
               "Capstone: ${CAPSTONE_FOUND}")

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
