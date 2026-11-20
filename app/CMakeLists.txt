# Target nexa_assistant, binary nexa-assistant: /usr/bin/nexa belongs to Nexa Download Manager.
qt_add_executable(nexa_assistant
    main.cc
    ActionLogDock.h ActionLogDock.cc
    AppController.h AppController.cc
    ChatView.h ChatView.cc
    GlobalShortcuts.h GlobalShortcuts.cc
    MainWindow.h MainWindow.cc
    MicrophoneLeveler.h MicrophoneLeveler.cc
    MicButton.h MicButton.cc
    PhoneLink.h PhoneLink.cc
    PhonePanel.h PhonePanel.cc
    RemoteAccess.h RemoteAccess.cc
    RemoteBridge.h RemoteBridge.cc
    SingleInstance.h SingleInstance.cc
    TrayIcon.h TrayIcon.cc
    SettingsDialog.h SettingsDialog.cc
    AccountSettingsPage.h AccountSettingsPage.cc
    ProviderGalleryDialog.h ProviderGalleryDialog.cc
    ProviderSignInBlock.h ProviderSignInBlock.cc
    BrowserSettingsPage.h BrowserSettingsPage.cc
    MemorySettingsPage.h MemorySettingsPage.cc
    SkillsSettingsPage.h SkillsSettingsPage.cc
    UnixSignals.h UnixSignals.cc
    ActivityStream.h ActivityStream.cc
    AddTasksDialog.h AddTasksDialog.cc
    AgentsRail.h AgentsRail.cc
    CoreCard.h CoreCard.cc
    MetricsTiles.h MetricsTiles.cc
    MissionBoard.h MissionBoard.cc
    MissionPanel.h MissionPanel.cc
    MissionSettingsPage.h MissionSettingsPage.cc
    TaskListModel.h TaskListModel.cc
    TaskStrip.h TaskStrip.cc
    TaskTranscriptDrawer.h TaskTranscriptDrawer.cc
    VoiceEnrolmentDialog.h VoiceEnrolmentDialog.cc
    VoiceSampleDialog.h VoiceSampleDialog.cc
)

set_target_properties(nexa_assistant PROPERTIES OUTPUT_NAME nexa-assistant)

# The window and tray icon, built in so it shows even before the desktop entry is installed.
set_source_files_properties("${PROJECT_SOURCE_DIR}/packaging/org.nexa.Assistant.svg"
    PROPERTIES QT_RESOURCE_ALIAS nexa.svg)
qt_add_resources(nexa_assistant "nexa_icons"
    PREFIX "/nexa"
    FILES "${PROJECT_SOURCE_DIR}/packaging/org.nexa.Assistant.svg"
)

# A packaged build finds Qt's plugins through this qt.conf, built in as :/qt/etc/qt.conf — the binary sits in
# /usr/bin and the plugins in /usr/lib/nexa-assistant/plugins. A source build must not carry it: the folder it
# names does not exist beside build/app.
if(NEXA_PACKAGED AND UNIX AND NOT ANDROID AND NOT APPLE)
    set_source_files_properties("${PROJECT_SOURCE_DIR}/packaging/qt.conf" PROPERTIES QT_RESOURCE_ALIAS qt.conf)
    qt_add_resources(nexa_assistant "nexa_qtconf"
        PREFIX "/qt/etc"
        FILES "${PROJECT_SOURCE_DIR}/packaging/qt.conf"
    )
endif()

if(WIN32)
    target_sources(nexa_assistant PRIVATE "${PROJECT_SOURCE_DIR}/packaging/nexa-assistant.rc")
    target_compile_definitions(nexa_assistant PRIVATE
        NEXA_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
        NEXA_VERSION_MINOR=${PROJECT_VERSION_MINOR}
        NEXA_VERSION_PATCH=${PROJECT_VERSION_PATCH}
    )
    set_target_properties(nexa_assistant PROPERTIES WIN32_EXECUTABLE TRUE)
    target_link_libraries(nexa_assistant PRIVATE user32)
endif()

install(TARGETS nexa_assistant RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")

target_link_libraries(nexa_assistant PRIVATE
    nexa_core
    nexa_account
    nexa_system
    nexa_voice
    nexa_remote
    nexa_ui
    nexa_options
    Qt6::Widgets
)

if(NEXA_HARDEN_RELEASE AND NOT NEXA_DEV_BUILD
   AND (CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "MinSizeRel"))
    set_target_properties(nexa_assistant PROPERTIES
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON
    )
    if(NOT MSVC)
        target_link_options(nexa_assistant PRIVATE -s)
    endif()
    include(CheckIPOSupported)
    check_ipo_supported(RESULT NEXA_IPO_OK OUTPUT NEXA_IPO_WHY LANGUAGES CXX)
    if(NEXA_IPO_OK)
        set_target_properties(nexa_assistant PROPERTIES INTERPROCEDURAL_OPTIMIZATION TRUE)
        message(STATUS "Release hardening + LTO/IPO enabled on nexa-assistant")
    else()
        message(STATUS "Release hardening enabled (LTO unavailable: ${NEXA_IPO_WHY})")
    endif()
endif()
