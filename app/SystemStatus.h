#pragma once

#include <QString>
#include <QStringList>

#include <functional>

namespace nexa {

class AdbDiscovery;
class AndroidDevice;
class GlobalShortcuts;
class HelperClient;
class PairingManager;
class RemoteServer;

// What the GUI needs to show about the parts of Nexa that live outside it: the root helper,
// whichever backend can type and click, and the phone. All of them can change while Nexa runs —
// installing the helper switches the input backend, a phone comes and goes — so these are looked
// up when shown, not copied.
struct SystemStatus {
    HelperClient *helper{nullptr};
    std::function<QString()> inputSummary;      // one line for the status bar
    std::function<QStringList()> inputNotes;    // every candidate and why it was or was not used
    AndroidDevice *phone{nullptr};
    AdbDiscovery *discovery{nullptr};
    RemoteServer *remote{nullptr};
    PairingManager *pairing{nullptr};
    GlobalShortcuts *shortcuts{nullptr};
};

} // namespace nexa
