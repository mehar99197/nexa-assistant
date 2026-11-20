#pragma once

#include <functional>

namespace nexa {

class IInputBackend;
class IScreenReader;
class ToolRegistry;

struct ScreenServices {
    IScreenReader *reader{nullptr};
    // The backend that types and presses keys, looked up when a tool runs (the root helper can be installed meanwhile).
    std::function<IInputBackend *()> input;
    // Share screen is on: screen_wait_for stops polling the moment it is switched off.
    std::function<bool()> allowed;
};

// Registers the desktop-as-text tools — screen_snapshot, screen_read, screen_click, screen_type, screen_wait_for —
// over the accessibility bus. The snapshot's refs (s1, s2, …) stay the current ones until the next snapshot; the
// acting tools take them. Offered to the model only while Share screen is on (AppController withholds them with
// take_screenshot).
void registerScreenTools(ToolRegistry &registry, const ScreenServices &services);

} // namespace nexa
