#pragma once

#include <functional>

namespace nexa {

class DesktopEntryIndex;
class FileAccess;
class IInputBackend;
class LocalCommandRunner;
class MprisController;
class RootCommandRunner;
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
    RootCommandRunner *rootCommands{nullptr};
    // Looked up when a tool runs, not when it is registered: which backend can type depends on
    // whether the root helper is installed, and that can change while Nexa is running.
    std::function<IInputBackend *()> input;
};

// Registers the PC tools: run_command, run_root_command, open_app, close_app, list_apps,
// get_volume, set_volume, mute, media_control, take_screenshot, read_file, write_file,
// get_system_info, type_text, press_keys, mouse_move, mouse_move_abs, mouse_click, mouse_scroll.
void registerSystemTools(ToolRegistry &registry, const SystemServices &services);

} // namespace nexa
