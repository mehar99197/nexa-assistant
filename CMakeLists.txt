cmake_minimum_required(VERSION 3.16)

project(Nexa
    VERSION 0.1.0
    DESCRIPTION "Linux AI assistant (C++20, Qt 6 Widgets)"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(NEXA_BUILD_TESTS "Build the Catch2 unit tests" ON)
option(NEXA_BUILD_WHISPER "Build local speech recognition with whisper.cpp (downloads it at configure time)" OFF)
option(NEXA_WERROR "Treat compiler warnings as errors" OFF)
option(NEXA_SANITIZE "Build with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)

find_package(Qt6 6.6 REQUIRED COMPONENTS Core Gui Widgets Network DBus Multimedia TextToSpeech)
qt_standard_project_setup(REQUIRES 6.6)

# Compile settings shared by every Nexa target (never applied to third-party code).
# Designated initializers deliberately omit defaulted members, hence -Wno-missing-field-initializers.
add_library(nexa_options INTERFACE)
target_compile_options(nexa_options INTERFACE -Wall -Wextra -Wpedantic -Wno-missing-field-initializers
    $<$<BOOL:${NEXA_WERROR}>:-Werror>)
if(NEXA_SANITIZE)
    target_compile_options(nexa_options INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(nexa_options INTERFACE -fsanitize=address,undefined)
endif()
target_compile_definitions(nexa_options INTERFACE
    QT_NO_URL_CAST_FROM_STRING
    QT_NO_NARROWING_CONVERSIONS_IN_CONNECT
    NEXA_VERSION="${PROJECT_VERSION}")

if(NEXA_BUILD_WHISPER)
    include(cmake/WhisperCpp.cmake)
endif()

# Libraries join the build when their milestone starts (see ARCHITECTURE.md → Status).
add_subdirectory(core)
add_subdirectory(system)
add_subdirectory(voice)
add_subdirectory(app)

if(NEXA_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
