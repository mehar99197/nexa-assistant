#pragma once

#include <QString>
#include <QStringList>

#include <memory>

namespace nexa {

class HelperClient;
class IInputBackend;

// Picks how Nexa will type and click, and — just as important — can say why. "Nothing happened
// when it typed" is the kind of problem that needs a reason, not silence: on Wayland only the root
// helper reaches native windows, so without it typing quietly goes nowhere.
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

    // `helper` may be null, or simply not ready; the selector checks before choosing it.
    static Choice choose(HelperClient *helper);
};

} // namespace nexa
