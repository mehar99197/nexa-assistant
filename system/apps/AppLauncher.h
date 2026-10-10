#pragma once

#include "core/Result.h"
#include "system/apps/DesktopEntry.h"

#include <QList>
#include <QString>

namespace nexa {

// Starts and stops desktop applications.
class AppLauncher {
public:
    // Plain entries are exec'd directly (fastest); D-Bus-activatable and terminal apps go through
    // `gio launch`, which knows both. Returns e.g. "Launched Firefox ESR (firefox-esr.desktop)".
    static Result<QString> launch(const DesktopEntry &entry);
    // SIGTERM to the user's processes running the entry's executable (`flatpak kill` for Flatpaks).
    static Result<QString> close(const DesktopEntry &entry);
    // PIDs of the current user's processes whose name, executable or argv[0] is `executable`.
    static QList<qint64> findProcesses(const QString &executable);
};

} // namespace nexa
