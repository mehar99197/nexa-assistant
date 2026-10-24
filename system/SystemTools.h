#pragma once

#include "core/agent/AppContext.h"

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
    // The application the user is in, as far as Nexa can tell (core's AppContext): open_app and
    // close_app report here, so the request after goes to the right place.
    std::function<void(const OpenedApp &)> appOpened;
    std::function<void(const OpenedApp &)> appClosed;
};

// Registers the PC tools: run_command, run_root_command, open_app, close_app, list_apps,
// get_volume, set_volume, mute, media_control, take_screenshot, read_file, write_file,
// get_system_info, type_text, press_keys, mouse_move, mouse_move_abs, mouse_click, mouse_scroll.
void registerSystemTools(ToolRegistry &registry, const SystemServices &services);

} // namespace nexa
