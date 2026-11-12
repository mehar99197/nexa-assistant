add_library(nexa_system STATIC
    SystemTools.h SystemTools.cc
    a11y/AtspiNode.h a11y/AtspiNode.cc
    a11y/ScreenOutline.h a11y/ScreenOutline.cc
    android/AdbClient.h android/AdbClient.cc
    android/AdbDiscovery.h android/AdbDiscovery.cc
    android/AdbParsers.h android/AdbParsers.cc
    android/AdbShell.h android/AdbShell.cc
    android/AndroidDevice.h android/AndroidDevice.cc
    android/AndroidTools.h android/AndroidTools.cc
    android/MdnsBrowser.h android/MdnsBrowser.cc
    android/MdnsPacket.h android/MdnsPacket.cc
    android/PhoneLookup.h android/PhoneLookup.cc
    android/PhoneMirror.h android/PhoneMirror.cc
    apps/AppLauncher.h apps/AppLauncher.cc
    apps/DesktopEntry.h apps/DesktopEntry.cc
    apps/DesktopEntryIndex.h apps/DesktopEntryIndex.cc
    audio/WpctlAudioController.h audio/WpctlAudioController.cc
    browser/BrowserService.h browser/BrowserService.cc
    browser/ChromeProfiles.h browser/ChromeProfiles.cc
    browser/PlaywrightInstaller.h browser/PlaywrightInstaller.cc
    commands/LocalCommandRunner.h commands/LocalCommandRunner.cc
    files/FileAccess.h files/FileAccess.cc
    files/AttachmentLoader.h files/AttachmentLoader.cc
    helper/HelperClient.h helper/HelperClient.cc
    commands/RootCommandRunner.h commands/RootCommandRunner.cc
    input/IInputBackend.h
    input/CombinedInputBackend.h input/CombinedInputBackend.cc
    input/HelperInputBackend.h input/HelperInputBackend.cc
    input/HelperInputLock.h input/HelperInputLock.cc
    input/InputBackendSelector.h input/InputBackendSelector.cc
    input/PortalInputBackend.h input/PortalInputBackend.cc
    input/RemoteDesktopSession.h input/RemoteDesktopSession.cc
    input/XTestInputBackend.h input/XTestInputBackend.cc
    info/ProcessStats.h info/ProcessStats.cc
    info/SystemInfo.h info/SystemInfo.cc
    media/MprisController.h media/MprisController.cc
    office/OdfWriter.h office/OdfWriter.cc
    office/OfficeTools.h office/OfficeTools.cc
    net/TailscaleWatcher.h net/TailscaleWatcher.cc
    net/TunnelLog.h net/TunnelLog.cc
    net/TunnelProcess.h net/TunnelProcess.cc
    screen/ScreenAwake.h screen/ScreenAwake.cc
    screen/ScreenCapture.h screen/ScreenCapture.cc
)

# X11/XTEST is the fallback input backend, used on X11 sessions and for XWayland windows.
find_package(X11 REQUIRED COMPONENTS Xtst)

target_link_libraries(nexa_system
    PUBLIC
        nexa_core
        nexa_proto
        Qt6::Gui
        Qt6::DBus
    PRIVATE
        nexa_options
        X11::X11
        X11::Xtst
)
