add_library(nexa_system STATIC
    SystemTools.h SystemTools.cc
    apps/AppLauncher.h apps/AppLauncher.cc
    apps/DesktopEntry.h apps/DesktopEntry.cc
    apps/DesktopEntryIndex.h apps/DesktopEntryIndex.cc
    audio/WpctlAudioController.h audio/WpctlAudioController.cc
    commands/LocalCommandRunner.h commands/LocalCommandRunner.cc
    files/FileAccess.h files/FileAccess.cc
    info/SystemInfo.h info/SystemInfo.cc
    media/MprisController.h media/MprisController.cc
    screen/ScreenCapture.h screen/ScreenCapture.cc
)

target_link_libraries(nexa_system
    PUBLIC
        nexa_core
        Qt6::Gui
        Qt6::DBus
    PRIVATE
        nexa_options
)
