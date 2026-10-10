#pragma once

namespace nexa {

class DesktopEntryIndex;
class FileAccess;
class LocalCommandRunner;
class MprisController;
class ScreenCapture;
class ToolRegistry;
class WpctlAudioController;

// The services behind the PC tools; owned by the composition root, which outlives the registry.
struct SystemServices {
    LocalCommandRunner *commands{nullptr};
    DesktopEntryIndex *apps{nullptr};
    WpctlAudioController *audio{nullptr};
    MprisController *media{nullptr};
    FileAccess *files{nullptr};
    ScreenCapture *screen{nullptr};
};

// Registers the PC tools: run_command, open_app, close_app, list_apps, get_volume, set_volume,
// mute, media_control, take_screenshot, read_file, write_file, get_system_info.
void registerSystemTools(ToolRegistry &registry, const SystemServices &services);

} // namespace nexa
