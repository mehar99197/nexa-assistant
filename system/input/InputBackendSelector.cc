#include "system/input/InputBackendSelector.h"

#include "core/Logging.h"
#include "system/helper/HelperClient.h"
#include "system/input/CombinedInputBackend.h"
#include "system/input/HelperInputBackend.h"
#include "system/input/PortalInputBackend.h"
#include "system/input/RemoteDesktopSession.h"
#include "system/input/XTestInputBackend.h"

#include <QCoreApplication>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

QString translated(const char *text) {
    return QCoreApplication::translate("InputBackendSelector", text);
}

} // namespace

InputBackendSelector::Choice InputBackendSelector::choose(HelperClient *helper, RemoteDesktopSession *remoteDesktop) {
    Choice choice;
    const bool wayland = XTestInputBackend::isWaylandSession();
    // Only worth asking about on Wayland: on X11, XTEST places the pointer exactly by itself.
    const bool portal = wayland && remoteDesktop && RemoteDesktopSession::isAvailable();

    // Installed is enough to choose it: the daemon itself is started by the first call that needs
    // it, so Nexa does not run root code just because it was launched.
    if (helper && (helper->isReady() || HelperClient::isInstalled())) {
        auto helperBackend = std::make_unique<HelperInputBackend>(helper);
        if (portal) {
            choice.backend = std::make_unique<CombinedInputBackend>(std::move(helperBackend),
                                                                    std::make_unique<PortalInputBackend>(remoteDesktop));
            choice.notes.append(translated("Using the root helper for typing and clicking: it reaches every window."));
            choice.notes.append(translated("Pointing at a spot goes through the desktop's remote-control portal, which "
                                           "GNOME asks you to allow once; the screen-sharing indicator shows while it "
                                           "is in use."));
            choice.summary = translated("Typing and clicking: root helper + desktop portal");
            qCInfo(lcApp) << "input backend: root helper, pointer placed by the RemoteDesktop portal";
            return choice;
        }
        choice.backend = std::move(helperBackend);
        choice.notes.append(translated("Using the root helper: it reaches every window."));
        if (wayland) {
            choice.notes.append(translated("The desktop offers no remote-control portal, so putting the pointer on an "
                                           "exact spot may not work."));
        }
        choice.summary = translated("Typing and clicking: root helper");
        qCInfo(lcApp) << "input backend: root helper";
        return choice;
    }

    const QString helperReason = helper && !helper->unavailableReason().isEmpty()
        ? helper->unavailableReason()
        : translated("it is not installed (%1)").arg(HelperClient::installCommand());
    choice.notes.append(translated("Root helper: not used — %1").arg(helperReason));

    if (portal) {
        choice.backend = std::make_unique<PortalInputBackend>(remoteDesktop);
        choice.notes.append(translated("Using the desktop's remote-control portal: it reaches every window once GNOME "
                                       "has asked you to allow it, and shows the screen-sharing indicator while in "
                                       "use."));
        choice.summary = translated("Typing and clicking: desktop portal");
        qCInfo(lcApp) << "input backend: RemoteDesktop portal";
        return choice;
    }
    if (wayland) choice.notes.append(translated("Desktop portal: this desktop does not offer remote control."));

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
