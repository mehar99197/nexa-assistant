#include "system/input/InputBackendSelector.h"

#include "core/Logging.h"
#include "system/helper/HelperClient.h"
#include "system/input/HelperInputBackend.h"
#include "system/input/XTestInputBackend.h"

#include <QCoreApplication>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

QString translated(const char *text) {
    return QCoreApplication::translate("InputBackendSelector", text);
}

} // namespace

InputBackendSelector::Choice InputBackendSelector::choose(HelperClient *helper) {
    Choice choice;
    const bool wayland = XTestInputBackend::isWaylandSession();

    // Installed is enough to choose it: the daemon itself is started by the first call that needs
    // it, so Nexa does not run root code just because it was launched.
    if (helper && (helper->isReady() || HelperClient::isInstalled())) {
        choice.backend = std::make_unique<HelperInputBackend>(helper);
        choice.notes.append(translated("Using the root helper: it reaches every window."));
        choice.summary = translated("Typing and clicking: root helper");
        qCInfo(lcApp) << "input backend: root helper";
        return choice;
    }

    const QString helperReason = helper && !helper->unavailableReason().isEmpty()
        ? helper->unavailableReason()
        : translated("it is not installed (%1)").arg(HelperClient::installCommand());
    choice.notes.append(translated("Root helper: not used — %1").arg(helperReason));

    QString xtestReason;
    if (XTestInputBackend::isUsable(&xtestReason)) {
        choice.backend = std::make_unique<XTestInputBackend>();
        if (wayland) {
            // Worth saying plainly: this backend will look broken for most windows.
            choice.notes.append(translated("Using XTEST, which on a Wayland session only reaches X11 windows. "
                                           "Typing into a native Wayland window will appear to do nothing — "
                                           "install the root helper for that."));
            choice.summary = translated("Typing and clicking: XTEST (X11 windows only)");
        } else {
            choice.notes.append(translated("Using XTEST on an X11 session: it reaches every window."));
            choice.summary = translated("Typing and clicking: XTEST");
        }
        qCInfo(lcApp) << "input backend: XTEST" << (wayland ? "(X11 windows only)" : "");
        return choice;
    }

    choice.notes.append(translated("XTEST: not usable — %1").arg(xtestReason));
    choice.summary = translated("Typing and clicking: unavailable");
    qCInfo(lcApp) << "no input backend:" << choice.notes;
    return choice;
}

} // namespace nexa
