#pragma once

#include <QString>

namespace nexa {

// Where a program Nexa drives (agy, opencode) is: `configured` when the user named one (and it can
// be run), otherwise the PATH, then ~/.local/bin, where installers for one user put programs and
// which a desktop session's PATH often lacks. Empty when it is not there.
QString findProgram(const QString &name, const QString &configured = {});

} // namespace nexa
