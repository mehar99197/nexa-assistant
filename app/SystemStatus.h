#pragma once

#include <QString>
#include <QStringList>

#include <functional>

namespace nexa {

class HelperClient;

// What the GUI needs to show about the parts of Nexa that live outside it: the root helper and
// whichever backend can type and click. Both can change while Nexa runs — installing the helper
// switches the input backend — so these are looked up when shown, not copied.
struct SystemStatus {
    HelperClient *helper{nullptr};
    std::function<QString()> inputSummary;      // one line for the status bar
    std::function<QStringList()> inputNotes;    // every candidate and why it was or was not used
};

} // namespace nexa
