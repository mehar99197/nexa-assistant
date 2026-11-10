cmake_minimum_required(VERSION 3.22)

# Nexa Phone: Nexa's own app for the phone — the web remote's client, native. It is built two ways:
# - inside Nexa's own build, for the desktop, so the screens can be tried and tested against a Nexa
#   on the same machine (`./build/android/nexa-phone`);
# - on its own with the Qt for Android kit, for the APK (README → "The phone app"):
#     cmake -S android -B build-android -DCMAKE_TOOLCHAIN_FILE=…/android_arm64_v8a/lib/cmake/Qt6/qt.toolchain.cmake …
project(NexaPhone VERSION 0.1 LANGUAGES CXX)

set(NEXA_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(NEXA_PHONE_STANDALONE OFF)
if(NOT TARGET nexa_remote)
    set(NEXA_PHONE_STANDALONE ON)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
    find_package(Qt6 6.6 REQUIRED COMPONENTS Core Gui Widgets Network WebSockets Multimedia)
    qt_standard_project_setup()
endif()

# The screens and what they need, as a library so the tests can drive them.
add_library(nexa_phone_ui STATIC
    ChatPage.h ChatPage.cc
    LaunchIntent.h LaunchIntent.cc
    MessageDelegate.h MessageDelegate.cc
    PairPage.h PairPage.cc
    PhoneSettings.h PhoneSettings.cc
    PhoneWindow.h PhoneWindow.cc
    Recorder.h Recorder.cc
    SpeechPlayer.h SpeechPlayer.cc
)
target_include_directories(nexa_phone_ui PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" "${NEXA_ROOT}")
target_link_libraries(nexa_phone_ui PUBLIC
    Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network Qt6::WebSockets Qt6::Multimedia
)
if(NEXA_PHONE_STANDALONE)
    # Shared with the PC side, so the two ends cannot drift: the wire format and the client, Nexa's
    # WAV decoder for the spoken reply, its audio converter for the microphone, and the look — the
    # Nexa core, the theme and the icons (ui/), which depend on Qt Widgets alone.
    target_sources(nexa_phone_ui PRIVATE
        "${NEXA_ROOT}/core/Logging.h" "${NEXA_ROOT}/core/Logging.cc"
        "${NEXA_ROOT}/remote/RemoteClient.h" "${NEXA_ROOT}/remote/RemoteClient.cc"
        "${NEXA_ROOT}/remote/RemoteProtocol.h" "${NEXA_ROOT}/remote/RemoteProtocol.cc"
        "${NEXA_ROOT}/remote/RemoteRoutes.h" "${NEXA_ROOT}/remote/RemoteRoutes.cc"
        "${NEXA_ROOT}/voice/Wav.h" "${NEXA_ROOT}/voice/Wav.cc"
        "${NEXA_ROOT}/voice/capture/AudioConverter.h" "${NEXA_ROOT}/voice/capture/AudioConverter.cc"
        "${NEXA_ROOT}/ui/ElidedLabel.h" "${NEXA_ROOT}/ui/ElidedLabel.cc"
        "${NEXA_ROOT}/ui/NexaCorePainter.h" "${NEXA_ROOT}/ui/NexaCorePainter.cc"
        "${NEXA_ROOT}/ui/NexaCoreSpec.h" "${NEXA_ROOT}/ui/NexaCoreSpec.cc"
        "${NEXA_ROOT}/ui/NexaCoreWidget.h" "${NEXA_ROOT}/ui/NexaCoreWidget.cc"
        "${NEXA_ROOT}/ui/NexaDesigns.h" "${NEXA_ROOT}/ui/NexaDesigns.cc"
        "${NEXA_ROOT}/ui/NexaIcons.h" "${NEXA_ROOT}/ui/NexaIcons.cc"
        "${NEXA_ROOT}/ui/NexaTheme.h" "${NEXA_ROOT}/ui/NexaTheme.cc"
        "${NEXA_ROOT}/ui/NexaThemes.h" "${NEXA_ROOT}/ui/NexaThemes.cc"
    )
    set_source_files_properties("${NEXA_ROOT}/ui/theme.qss" PROPERTIES QT_RESOURCE_ALIAS theme.qss)
    set_source_files_properties("${NEXA_ROOT}/ui/chevron-down.svg" PROPERTIES QT_RESOURCE_ALIAS chevron-down.svg)
    set_source_files_properties("${NEXA_ROOT}/ui/check.svg" PROPERTIES QT_RESOURCE_ALIAS check.svg)
    qt_add_resources(nexa_phone_ui "nexa_theme"
        PREFIX "/nexa/ui"
        FILES "${NEXA_ROOT}/ui/theme.qss" "${NEXA_ROOT}/ui/chevron-down.svg" "${NEXA_ROOT}/ui/check.svg"
    )
    target_compile_options(nexa_phone_ui PRIVATE -Wall -Wextra -Wpedantic -Wno-missing-field-initializers)
else()
    target_link_libraries(nexa_phone_ui PUBLIC nexa_remote nexa_voice nexa_ui PRIVATE nexa_options)
endif()

qt_add_executable(nexa-phone main.cc)
target_link_libraries(nexa-phone PRIVATE nexa_phone_ui)
if(NOT NEXA_PHONE_STANDALONE)
    target_link_libraries(nexa-phone PRIVATE nexa_options)
endif()

if(ANDROID)
    set_target_properties(nexa-phone PROPERTIES
        QT_ANDROID_PACKAGE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/manifest"
        QT_ANDROID_PACKAGE_NAME "org.nexa.phone"
        QT_ANDROID_APP_NAME "Nexa"
        QT_ANDROID_APP_ICON "@drawable/icon"
        QT_ANDROID_VERSION_NAME "${PROJECT_VERSION}"
        QT_ANDROID_VERSION_CODE 1
        QT_ANDROID_MIN_SDK_VERSION 28
        # Qt 6.11's Android template brings androidx.core 1.17, which needs API 36 to compile against.
        QT_ANDROID_TARGET_SDK_VERSION 36
    )
    # Only what it does: reach Nexa, and hear the user while Talk is held.
    qt_add_android_permission(nexa-phone NAME android.permission.INTERNET)
    qt_add_android_permission(nexa-phone NAME android.permission.RECORD_AUDIO)

    # TLS. Qt for Android ships its OpenSSL plugin but not OpenSSL: the APK has to carry libcrypto_3.so
    # and libssl_3.so for the ABI, which the plugin loads by exactly those names. Without them the app
    # has no TLS at all — every https:// and wss:// address ends in "TLS initialization failed", and
    # only http://localhost through adb works — so a build without them is refused here rather than
    # found out on the phone. scripts/download-android-openssl.sh fetches them (KDAB's prebuilt ones).
    set(NEXA_ANDROID_OPENSSL_DIR "$ENV{HOME}/Android/android_openssl" CACHE PATH
        "OpenSSL built for Android, in KDAB's layout: ssl_3/<abi>/libcrypto_3.so and libssl_3.so")
    set(nexa_openssl_abi "${CMAKE_ANDROID_ARCH_ABI}")
    if(NOT nexa_openssl_abi)
        set(nexa_openssl_abi "${ANDROID_ABI}")
    endif()
    set(nexa_openssl_libs
        "${NEXA_ANDROID_OPENSSL_DIR}/ssl_3/${nexa_openssl_abi}/libcrypto_3.so"
        "${NEXA_ANDROID_OPENSSL_DIR}/ssl_3/${nexa_openssl_abi}/libssl_3.so"
    )
    foreach(lib IN LISTS nexa_openssl_libs)
        if(NOT EXISTS "${lib}")
            message(FATAL_ERROR "OpenSSL for Android is missing: ${lib}\n"
                "Run scripts/download-android-openssl.sh, or point NEXA_ANDROID_OPENSSL_DIR at a copy; "
                "without it the phone app cannot use HTTPS.")
        endif()
    endforeach()
    set_property(TARGET nexa-phone APPEND PROPERTY QT_ANDROID_EXTRA_LIBS ${nexa_openssl_libs})
endif()
