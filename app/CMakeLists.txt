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
    RemoteBridge.h RemoteBridge.cc
    SingleInstance.h SingleInstance.cc
    TrayIcon.h TrayIcon.cc
    SettingsDialog.h SettingsDialog.cc
    BrowserSettingsPage.h BrowserSettingsPage.cc
    MemorySettingsPage.h MemorySettingsPage.cc
    SkillsSettingsPage.h SkillsSettingsPage.cc
    UnixSignals.h UnixSignals.cc
)

set_target_properties(nexa_assistant PROPERTIES OUTPUT_NAME nexa-assistant)

# The window and tray icon, built in so it shows even before the desktop entry is installed.
set_source_files_properties("${PROJECT_SOURCE_DIR}/packaging/org.nexa.Assistant.svg"
    PROPERTIES QT_RESOURCE_ALIAS nexa.svg)
qt_add_resources(nexa_assistant "nexa_icons"
    PREFIX "/nexa"
    FILES "${PROJECT_SOURCE_DIR}/packaging/org.nexa.Assistant.svg"
)

target_link_libraries(nexa_assistant PRIVATE
    nexa_core
    nexa_system
    nexa_voice
    nexa_remote
    nexa_ui
    nexa_options
    Qt6::Widgets
)
