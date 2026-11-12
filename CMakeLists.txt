cmake_minimum_required(VERSION 3.16)

project(Nexa
    VERSION 1.0.0
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
option(NEXA_BUILD_PHONE "Build the phone app for the desktop too (android/), so it can be tested here" ON)
option(NEXA_SANITIZE "Build with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)
# A packaged build (the .deb, scripts/build-deb.sh): Qt's plugins are found through the qt.conf built into the
# binary, and the speech engines in NEXA_ENGINE_DIR when the user has no copy of their own.
option(NEXA_PACKAGED "Build for the self-contained package: embedded qt.conf, engines under NEXA_ENGINE_DIR" OFF)
option(NEXA_BUILD_HELPER "Build the root helper (needs polkit-qt6); OFF for a build against another Qt without it" ON)

include(GNUInstallDirs)
set(CMAKE_INSTALL_DOCDIR "${CMAKE_INSTALL_DATAROOTDIR}/doc/nexa-assistant")
if(NEXA_PACKAGED)
    set(NEXA_ENGINE_DIR_DEFAULT "${CMAKE_INSTALL_FULL_LIBDIR}/nexa-assistant")
else()
    set(NEXA_ENGINE_DIR_DEFAULT "")
endif()
set(NEXA_ENGINE_DIR "${NEXA_ENGINE_DIR_DEFAULT}" CACHE STRING
    "Where the package keeps the speech engines (sherpa-onnx, Piper); empty: the user's data folder only")
# The root helper's installed path, written into its D-Bus service and systemd unit.
set(NEXA_HELPER_PATH "${CMAKE_INSTALL_FULL_LIBEXECDIR}/nexa-assistant/nexa-helper")

find_package(Qt6 6.6 REQUIRED COMPONENTS Core Gui Widgets Network DBus Multimedia TextToSpeech HttpServer WebSockets)
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
add_subdirectory(proto)
add_subdirectory(system)
add_subdirectory(voice)
add_subdirectory(remote)
add_subdirectory(ui)
add_subdirectory(app)
if(NEXA_BUILD_HELPER)
    add_subdirectory(helper)
endif()
if(NEXA_BUILD_PHONE)
    add_subdirectory(android)
endif()

if(NEXA_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------------------------
# The install layout (the .deb's; a source install uses packaging/install-helper.sh and
# scripts/install-desktop-entry.sh instead). The helper's service and unit carry its installed path.
# ---------------------------------------------------------------------------------------------
configure_file(packaging/org.nexa.Helper.service.in "${CMAKE_BINARY_DIR}/packaging/org.nexa.Helper.service" @ONLY)
configure_file(packaging/nexa-helper.service.in "${CMAKE_BINARY_DIR}/packaging/nexa-helper.service" @ONLY)
install(FILES "${CMAKE_BINARY_DIR}/packaging/org.nexa.Helper.service"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/dbus-1/system-services")
install(FILES packaging/org.nexa.Helper.conf DESTINATION /etc/dbus-1/system.d)
install(FILES packaging/org.nexa.helper.policy DESTINATION "${CMAKE_INSTALL_DATADIR}/polkit-1/actions")
install(FILES "${CMAKE_BINARY_DIR}/packaging/nexa-helper.service" DESTINATION "${CMAKE_INSTALL_LIBDIR}/systemd/system")
install(FILES packaging/org.nexa.Assistant.desktop DESTINATION "${CMAKE_INSTALL_DATADIR}/applications")
install(FILES packaging/org.nexa.Assistant.svg DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps")
install(FILES README.md THIRD_PARTY_NOTICES.md LICENSE DESTINATION "${CMAKE_INSTALL_DOCDIR}")
