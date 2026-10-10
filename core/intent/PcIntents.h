#pragma once

#include "core/intent/IIntentHandler.h"

#include <functional>

namespace nexa {

// Fast-path handlers for the PC, in the order the router should try them.
// (Small classes kept in one file; each is one link of the IntentRouter chain.)

// "stop", "cancel", "never mind" → panic.
class ControlIntent : public IIntentHandler {
public:
    std::optional<IntentMatch> match(const IntentInput &input) const override;
};

// "play/pause/next/previous/stop music" → media_control.
class MediaIntent : public IIntentHandler {
public:
    std::optional<IntentMatch> match(const IntentInput &input) const override;
};

// "volume 30", "volume up by 10", "louder", "mute" → set_volume / mute.
class AudioIntent : public IIntentHandler {
public:
    std::optional<IntentMatch> match(const IntentInput &input) const override;
};

// "screenshot", "see this", "what's on my screen" → take_screenshot, then the model answers.
class ScreenIntent : public IIntentHandler {
public:
    std::optional<IntentMatch> match(const IntentInput &input) const override;
};

// "open firefox", "close the terminal" → open_app / close_app, when the catalog knows the app.
class AppIntent : public IIntentHandler {
public:
    static constexpr double kMinScore = 0.8;
    explicit AppIntent(const IAppCatalog *catalog) : catalog_(catalog) {}
    std::optional<IntentMatch> match(const IntentInput &input) const override;

private:
    const IAppCatalog *catalog_;
};

// "run ls -la", "$ df -h", "run htop as root" → run_command / run_root_command.
// "run <x>" only matches when <x> is a real program (`commandExists`), so chat like
// "run me through it" still reaches the model; "$ <x>" always matches.
class CommandIntent : public IIntentHandler {
public:
    using CommandExists = std::function<bool(const QString &program)>;
    CommandIntent(const IAppCatalog *catalog, CommandExists commandExists)
        : catalog_(catalog), commandExists_(std::move(commandExists)) {}
    std::optional<IntentMatch> match(const IntentInput &input) const override;

private:
    const IAppCatalog *catalog_;
    CommandExists commandExists_;
};

// "type hello world", "press ctrl+alt+t" → type_text / press_keys.
class InputIntent : public IIntentHandler {
public:
    std::optional<IntentMatch> match(const IntentInput &input) const override;
};

} // namespace nexa
