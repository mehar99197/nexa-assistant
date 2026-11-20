#pragma once

#include <QProcessEnvironment>
#include <QString>
#include <QStringList>

namespace nexa {

// Where a program Nexa drives (agy, opencode, codex) is: `configured` when the user named one (used as given: empty
// when it cannot be run), otherwise the PATH, then the folders per-user installers put programs in, which a desktop
// session's PATH often lacks (programSearchDirs). Empty when it is nowhere.
QString findProgram(const QString &name, const QString &configured = {});

// The per-user folders, in the order they are searched: ~/.local/bin, ~/.npm-global/bin, npm's prefix from ~/.npmrc,
// every node of nvm (the newest first), Volta's and Bun's, then /usr/local/bin.
QStringList programSearchDirs(const QString &home);
QString findProgramIn(const QString &name, const QStringList &dirs);
// Where findProgram looks, in words, for a "not installed" message.
QString programSearchHint();

// The environment to start `program` in: Nexa's own, with the program's folder first on the PATH — an npm program
// is a `#!/usr/bin/env node` script, and under nvm its node lives beside it, not on the desktop's PATH.
QProcessEnvironment environmentFor(const QString &program);

} // namespace nexa
