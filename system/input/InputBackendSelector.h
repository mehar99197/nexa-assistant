#pragma once

#include <QString>
#include <QStringList>

#include <memory>

namespace nexa {

class HelperClient;
class IInputBackend;
class RemoteDesktopSession;

// Picks how Nexa will type and click, and — just as important — can say why. "Nothing happened
// when it typed" is the kind of problem that needs a reason, not silence: on Wayland, XTEST only
// reaches X11 windows, so typing through it quietly goes nowhere for most of the desktop.
//
// On Wayland, in order: the root helper for typing and clicking with the desktop's RemoteDesktop
// portal placing the pointer; the portal alone when the helper is not installed; XTEST last.
//
// Call choose() again after the helper is installed or its probe finishes; the selector holds no
// state of its own.
class InputBackendSelector {
public:
    struct Choice {
        std::unique_ptr<IInputBackend> backend;     // null when nothing can inject input
        QString summary;                            // one line for the status bar
        QStringList notes;                          // every candidate and why it was or was not used
    };

    // Either may be null. `helper` may simply not be ready; the selector checks before choosing it.
    static Choice choose(HelperClient *helper, RemoteDesktopSession *remoteDesktop);
};

} // namespace nexa
