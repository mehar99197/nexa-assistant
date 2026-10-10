# Target nexa_assistant, binary nexa-assistant: /usr/bin/nexa belongs to Nexa Download Manager.
qt_add_executable(nexa_assistant
    main.cc
    ActionLogDock.h ActionLogDock.cc
    AppController.h AppController.cc
    ChatView.h ChatView.cc
    GlobalShortcuts.h GlobalShortcuts.cc
    MainWindow.h MainWindow.cc
    MicButton.h MicButton.cc
    SettingsDialog.h SettingsDialog.cc
    UnixSignals.h UnixSignals.cc
)

set_target_properties(nexa_assistant PROPERTIES OUTPUT_NAME nexa-assistant)

target_link_libraries(nexa_assistant PRIVATE
    nexa_core
    nexa_system
    nexa_voice
    nexa_options
    Qt6::Widgets
)
